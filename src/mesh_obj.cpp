#include "mesh.h"
#include "light.h"
#include "mesh_loader.h"
#include "texture.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

namespace
{
    // True if the OBJ contains any `s` (smoothing-group) directive with a value, e.g.
    // `s 1` or `s off`. tinyobjloader collapses "no directive", `s 0`, and `s off` all
    // to group id 0, so the per-face ids alone can't distinguish "no smoothing info"
    // (→ crease-angle fallback) from "explicitly faceted" (`s off`). This scan decides
    // whether compute_normals runs in smoothing-group mode at all; it runs only when
    // the file lacks normals, and early-outs on the first directive.
    bool obj_has_smoothing_directive(const std::string &path)
    {
        std::ifstream in(path);
        if (!in)
        {
            return false;
        }
        std::string line;
        while (std::getline(in, line))
        {
            size_t i = 0;
            while (i < line.size() && (std::isspace(static_cast<unsigned char>(line[i])) != 0))
            {
                i++;
            }
            // First token must be exactly `s` followed by whitespace (not `surf` etc.).
            if (i >= line.size() || line[i] != 's')
            {
                continue;
            }
            size_t j = i + 1;
            if (j < line.size() && (std::isspace(static_cast<unsigned char>(line[j])) == 0))
            {
                continue;
            }
            // Require a following non-whitespace value token (skip a malformed bare `s`).
            while (j < line.size() && (std::isspace(static_cast<unsigned char>(line[j])) != 0))
            {
                j++;
            }
            if (j < line.size())
            {
                return true;
            }
        }
        return false;
    }
} // namespace

bool Mesh::load_obj(const std::string &path, int n_threads, float crease_cos)
{
    MeshSnapshot snap(*this);

    // Texture paths in MTL are resolved relative to the OBJ file's directory.
    const std::string obj_dir = dir_of(path);

    tinyobj::ObjReaderConfig cfg;
    cfg.mtl_search_path = obj_dir;
    cfg.triangulate = true;

    tinyobj::ObjReader reader;
    if (!reader.ParseFromFile(path, cfg))
    {
        return false;
    }

    const auto &attrib = reader.GetAttrib();
    const auto &shapes = reader.GetShapes();
    const auto &mats = reader.GetMaterials();

    // Bounds-check all face indices. tinyobjloader is permissive (warns but succeeds)
    // on OOB references; we reject to avoid degenerate geometry in the renderer.
    const size_t n_pos = attrib.vertices.size() / 3;
    const size_t n_nor = attrib.normals.size() / 3;
    const size_t n_tex = attrib.texcoords.size() / 2;
    for (const auto &shape : shapes)
    {
        for (const auto &idx : shape.mesh.indices)
        {
            if ((idx.vertex_index >= 0 && static_cast<size_t>(idx.vertex_index) >= n_pos) ||
                (idx.normal_index >= 0 && static_cast<size_t>(idx.normal_index) >= n_nor) ||
                (idx.texcoord_index >= 0 && static_cast<size_t>(idx.texcoord_index) >= n_tex))
            {
                return false;
            }
        }
    }

    // Default white material at index 0 — always present.
    materials.push_back(Material{});

    // Register a texture by name (relative to obj_dir), returning its slot index.
    // Each distinct (name, clamp) is registered once (dedup); the actual decode is
    // deferred and run in parallel after material parsing. Empty name -> no texture (-1).
    // clamp is MTL's `-clamp on` (both axes -> Clamp, no mirror); it is part of the key
    // because the same image used clamped in one material and tiled in another needs two
    // slots with different wrap modes.
    struct ObjTexRequest
    {
        std::string path;
        bool clamp;
    };
    std::unordered_map<std::string, int> tex_cache;
    std::vector<ObjTexRequest> tex_requests;
    auto load_tex = [&](const std::string &name, bool clamp) -> int
    {
        if (name.empty())
        {
            return -1;
        }
        const std::string key = (clamp ? "1:" : "0:") + name;
        const auto it = tex_cache.find(key);
        if (it != tex_cache.end())
        {
            return it->second;
        }
        const int idx = static_cast<int>(tex_requests.size());
        tex_requests.push_back({ obj_dir + name, clamp });
        tex_cache.emplace(key, idx);
        return idx;
    };

    // Materials: tinyobjloader index i maps to our index i+1 (0 = default).
    for (const auto &m : mats)
    {
        Material mat{};
        mat.diffuse = { m.diffuse[0], m.diffuse[1], m.diffuse[2] };
        mat.specular = { m.specular[0], m.specular[1], m.specular[2] };
        mat.shininess = m.shininess;
        // Ka → Kd fallback: if ambient is all-zero (absent or unset), use diffuse.
        const bool ka_zero = (m.ambient[0] == 0.0f && m.ambient[1] == 0.0f && m.ambient[2] == 0.0f);
        mat.ambient = ka_zero ? mat.diffuse : vec3{ m.ambient[0], m.ambient[1], m.ambient[2] };
        mat.diffuse_tex = load_tex(m.diffuse_texname, m.diffuse_texopt.clamp);
        mat.specular_tex = load_tex(m.specular_texname, m.specular_texopt.clamp);
        // Prefer map_Kn (normal_texname); fall back to map_bump (bump_texname).
        mat.normal_tex = !m.normal_texname.empty() ? load_tex(m.normal_texname, m.normal_texopt.clamp)
                                                   : load_tex(m.bump_texname, m.bump_texopt.clamp);
        // Clamp Ke to [0, 1e6] per channel: emission is physically non-negative (glTF enforces
        // the same via emissiveFactor's `minimum: 0.0`). Lower bound stops a negative from
        // subtracting from lit colour; upper bound stops a hostile +Inf at the source, before
        // it can produce a per-pixel NaN via `Inf * 0` on a zero texel channel (uint8_t cast on
        // NaN is UB). Symmetric with the glTF emissiveFactor clamp.
        mat.emissive = { std::clamp(m.emission[0], 0.0f, 1e6f), std::clamp(m.emission[1], 0.0f, 1e6f),
                         std::clamp(m.emission[2], 0.0f, 1e6f) };
        // Spec-literal: emissive = Ke × map_Ke. A zero Ke means map_Ke cannot contribute
        // (do_emissive gated on factor>0), so skip the decode — saves a stb_image_load (often
        // multi-MB) and permanent RAM. Dedup unaffected: if the same image is bound as e.g.
        // map_Kd, that call still registers and decodes it.
        const bool emissive_active = (mat.emissive.x > 0.0f || mat.emissive.y > 0.0f || mat.emissive.z > 0.0f);
        if (emissive_active)
        {
            mat.emissive_tex = load_tex(m.emissive_texname, m.emissive_texopt.clamp);
        }
        // map_d present: treat map_Kd's alpha channel as an opacity mask.
        // map_d is not loaded as a separate texture — map_Kd's RGBA is used.
        // Otherwise a sub-1 dissolve (MTL `d` / `Tr`, parsed into m.dissolve by
        // tinyobjloader) routes the material to the transparent blend pass. The two are
        // mutually exclusive: a cutout mask takes precedence over scalar opacity.
        if (!m.alpha_texname.empty())
        {
            mat.alpha_cutoff = 0.5f;
        }
        else if (m.dissolve < 1.0f)
        {
            mat.blend = true;
            // Authored dissolve passes straight through with no floor: `d 0` is a fully
            // invisible material, which is spec-correct (0 = fully dissolved). This is a
            // deliberate asymmetry with the glTF transmission path's GLASS_ALPHA_FLOOR --
            // that floor exists only because transmission->alpha-blend is an approximation
            // that would otherwise vanish a surface the asset meant to be seen; an explicit
            // `d`/`Tr` value is the author's literal intent, so it is not second-guessed.
            mat.alpha = m.dissolve;
        }
        materials.push_back(mat);
    }

    // Vertex deduplication: identical (vertex_index, normal_index, texcoord_index)
    // tuples share one Vertex — same semantics as the previous hand-rolled loader.
    std::unordered_map<size_t, uint32_t> vert_cache;
    vert_cache.reserve(attrib.vertices.size() / 3);

    bool all_have_normals = !attrib.normals.empty();

    // Position-group id per created vertex (the source OBJ vertex_index), so
    // compute_normals can smooth across UV seams that split one position into
    // several vertices. Only consumed when the file lacks (full) normals.
    std::vector<uint32_t> weld;
    weld.reserve(n_pos);

    auto get_vertex = [&](const tinyobj::index_t &idx) -> uint32_t
    {
        const size_t key = (static_cast<size_t>(idx.vertex_index + 1) * size_t{ 2654435761 }) ^
                           (static_cast<size_t>(idx.normal_index + 1) * size_t{ 2246822519 }) ^
                           (static_cast<size_t>(idx.texcoord_index + 1) * size_t{ 3266489917 });

        const auto [it, inserted] = vert_cache.emplace(key, static_cast<uint32_t>(vertices.size()));
        if (!inserted)
        {
            return it->second;
        }

        Vertex v{};
        if (idx.vertex_index >= 0)
        {
            const size_t vi = static_cast<size_t>(idx.vertex_index) * 3;
            v.pos = { attrib.vertices[vi], attrib.vertices[vi + 1], attrib.vertices[vi + 2] };
        }
        if (idx.normal_index >= 0)
        {
            const size_t ni = static_cast<size_t>(idx.normal_index) * 3;
            v.normal = { attrib.normals[ni], attrib.normals[ni + 1], attrib.normals[ni + 2] };
        }
        else
        {
            all_have_normals = false;
        }
        if (idx.texcoord_index >= 0)
        {
            const size_t ti = static_cast<size_t>(idx.texcoord_index) * 2;
            v.uv = { attrib.texcoords[ti], attrib.texcoords[ti + 1] };
        }
        v.ao = 1.0f;
        vertices.push_back(v);
        weld.push_back(idx.vertex_index >= 0 ? static_cast<uint32_t>(idx.vertex_index) : 0u);
        return it->second;
    };

    // Check for OBJ vertex colors ("v x y z r g b" extension).
    const bool src_has_vcol = (!attrib.colors.empty() && attrib.colors.size() == attrib.vertices.size());

    // Walk all shapes and merge into one Mesh.
    for (const auto &shape : shapes)
    {
        size_t idx_off = 0;
        for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); f++)
        {
            const int fv = static_cast<int>(shape.mesh.num_face_vertices[f]);
            const int mat_id = shape.mesh.material_ids[f];
            const uint32_t mat_idx =
                (mat_id >= 0 && mat_id < static_cast<int>(mats.size())) ? static_cast<uint32_t>(mat_id + 1) : 0u;

            // triangulate=true guarantees fv==3; guard for safety.
            if (fv >= 3)
            {
                std::array<uint32_t, 3> fv3{};
                for (int v = 0; v < 3; v++)
                {
                    fv3[static_cast<size_t>(v)] = get_vertex(shape.mesh.indices[idx_off + static_cast<size_t>(v)]);
                }

                Triangle t;
                t.v[0] = fv3[0];
                t.v[1] = fv3[1];
                t.v[2] = fv3[2];
                t.material_idx = mat_idx;
                triangles.push_back(t);
            }
            idx_off += static_cast<size_t>(fv);
        }
    }

    if (triangles.empty())
    {
        return false;
    }

    // Populate vertex colors when "v x y z r g b" data is present.
    if (src_has_vcol)
    {
        vertex_colors.assign(vertices.size(), { 1.0f, 1.0f, 1.0f });
        for (const auto &shape : shapes)
        {
            for (const auto &idx : shape.mesh.indices)
            {
                if (idx.vertex_index >= 0)
                {
                    const size_t ci = static_cast<size_t>(idx.vertex_index) * 3;
                    const size_t key = (static_cast<size_t>(idx.vertex_index + 1) * size_t{ 2654435761 }) ^
                                       (static_cast<size_t>(idx.normal_index + 1) * size_t{ 2246822519 }) ^
                                       (static_cast<size_t>(idx.texcoord_index + 1) * size_t{ 3266489917 });
                    const auto it = vert_cache.find(key);
                    if (it != vert_cache.end())
                    {
                        vertex_colors[it->second] = { attrib.colors[ci], attrib.colors[ci + 1], attrib.colors[ci + 2] };
                    }
                }
            }
        }
        has_vertex_colors = std::any_of(
            vertex_colors.begin(), vertex_colors.end(),
            [](const vec3 &c) { return c.x < 0.999f || c.y < 0.999f || c.z < 0.999f; }
        );
        if (!has_vertex_colors)
        {
            vertex_colors.clear();
        }
    }

    if (attrib.normals.empty() || !all_have_normals)
    {
        // OBJ smoothing groups, when authored, are authoritative over the crease
        // angle. Build a per-triangle group id (parallel to `triangles`) mirroring
        // the triangle-build loop's order exactly so the indices line up. Only
        // built when the file actually authors `s` directives — otherwise the
        // crease-angle path runs byte-identically (nullptr).
        //
        // Any non-zero id proves a directive exists, so the disk rescan is needed
        // only to disambiguate the all-zero case (no directive vs. explicit `s off`,
        // which tinyobjloader both map to 0): the former is the angle fallback, the
        // latter is faceted group mode.
        std::vector<uint32_t> smooth_groups;
        const bool any_nonzero = std::any_of(
            shapes.begin(), shapes.end(),
            [](const tinyobj::shape_t &s)
            {
                return std::any_of(
                    s.mesh.smoothing_group_ids.begin(), s.mesh.smoothing_group_ids.end(),
                    [](unsigned int id) { return id != 0u; }
                );
            }
        );
        const bool use_groups = any_nonzero || obj_has_smoothing_directive(path);
        if (use_groups)
        {
            for (const auto &shape : shapes)
            {
                for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); f++)
                {
                    if (shape.mesh.num_face_vertices[f] >= 3)
                    {
                        smooth_groups.push_back(shape.mesh.smoothing_group_ids[f]);
                    }
                }
            }
        }
        compute_normals(crease_cos, &weld, n_pos, use_groups ? &smooth_groups : nullptr);
    }

    // tex_requests holds obj_dir-resolved paths + the -clamp flag, decoded in parallel.
    decode_textures(
        textures, materials, tex_requests.size(), n_threads,
        [&](size_t i) -> Texture
        {
            Texture tex;
            (void)tex.load(tex_requests[i].path);
            if (tex_requests[i].clamp)
            {
                tex.wrap_s = WrapMode::Clamp;
                tex.wrap_t = WrapMode::Clamp;
            }
            return tex;
        }
    );

    snap.commit();
    return true;
}
