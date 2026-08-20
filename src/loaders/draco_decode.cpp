#include "src/loaders/draco_decode.h"

// Draco's templated decoder trips the project's strict warnings in its own
// headers (notably GCC -Wmaybe-uninitialized under LTO at template instantiation
// points). We don't audit vendored code (refresh from upstream instead), so
// suppress those here, where the Draco headers are confined. This file is the
// Draco analogue of vendor/meshoptimizer/meshoptimizer_impl.cpp.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

#include "draco/attributes/geometry_indices.h"
#include "draco/attributes/point_attribute.h"
#include "draco/compression/decode.h"
#include "draco/core/decoder_buffer.h"
#include "draco/mesh/mesh.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory> // IWYU pragma: keep, std::unique_ptr<draco::Mesh> below; clangd can't trace it through Draco's StatusOr::value()
#include <utility>

namespace
{
    const draco::PointAttribute *attr_or_null(const draco::Mesh &m, int id)
    {
        if (id < 0)
        {
            return nullptr;
        }
        return m.GetAttributeByUniqueId(static_cast<uint32_t>(id));
    }
} // namespace

bool decode_draco_mesh(
    const void *data, size_t size, uint32_t pos_id, int normal_id, int uv_id, int uv1_id, int color_id, DracoMesh &out
)
{
    draco::DecoderBuffer buf;
    buf.Init(static_cast<const char *>(data), size);
    draco::Decoder decoder;
    auto res = decoder.DecodeMeshFromBuffer(&buf);
    if (!res.ok())
    {
        return false;
    }
    const std::unique_ptr<draco::Mesh> mesh = std::move(res).value();

    const draco::PointAttribute *pos = mesh->GetAttributeByUniqueId(pos_id);
    if (!pos)
    {
        return false;
    }
    const draco::PointAttribute *nrm = attr_or_null(*mesh, normal_id);
    const draco::PointAttribute *uv = attr_or_null(*mesh, uv_id);
    const draco::PointAttribute *uv1 = attr_or_null(*mesh, uv1_id);
    const draco::PointAttribute *col = attr_or_null(*mesh, color_id);

    // Partial file-size bound (CLAUDE.md "Loader security"): rejects decoded counts that would
    // OOM our flat-float resize, but does NOT bound Draco's own allocations inside
    // DecodeMeshFromBuffer: a bitstream that inflates internally and only then reports small
    // counts can still bad_alloc inside Draco, and the public API exposes no header pre-parse
    // of the declared point count to close that. 200x the compressed input leaves generous
    // headroom over Draco's real-world ~5-10x expansion; the division form sidesteps overflow
    // at huge sizes.
    constexpr size_t MAX_EXPANSION = 200;
    const size_t n = mesh->num_points();
    const size_t nf = mesh->num_faces();
    if (size == 0 || n / MAX_EXPANSION > size || nf / MAX_EXPANSION > size)
    {
        return false;
    }
    // A 4-component COLOR_0 carries per-vertex opacity in its 4th channel; surface it
    // in colors_alpha. Mirrors the accessor path's strict vec4 test: a non-RGBA color
    // attribute (3, or a malformed >4) is not treated as carrying alpha.
    const bool col_alpha = col && col->num_components() == 4;

    out.num_points = n;
    out.positions.resize(n * 3);
    if (nrm)
    {
        out.normals.resize(n * 3);
    }
    if (uv)
    {
        out.uvs.resize(n * 2);
    }
    if (uv1)
    {
        out.uvs1.resize(n * 2);
    }
    if (col)
    {
        out.colors.resize(n * 3);
    }
    if (col_alpha)
    {
        out.colors_alpha.resize(n);
    }
    for (size_t i = 0; i < n; i++)
    {
        const draco::PointIndex pi(static_cast<uint32_t>(i));
        pos->ConvertValue<float>(pos->mapped_index(pi), 3, &out.positions[i * 3]);
        if (nrm)
        {
            nrm->ConvertValue<float>(nrm->mapped_index(pi), 3, &out.normals[i * 3]);
        }
        if (uv)
        {
            uv->ConvertValue<float>(uv->mapped_index(pi), 2, &out.uvs[i * 2]);
        }
        if (uv1)
        {
            uv1->ConvertValue<float>(uv1->mapped_index(pi), 2, &out.uvs1[i * 2]);
        }
        if (col)
        {
            // num_components() is an unconstrained uint8_t straight from the bitstream
            // (1-255), and ConvertValue writes that many floats into the output. Clamp
            // to the 4-float destination so a malformed COLOR_0 attribute declaring >4
            // components can't overflow this stack buffer (RGB into colors, the 4th into
            // colors_alpha when col_alpha). The clamp also keeps the int8_t cast lossless.
            float c[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            const auto nc = static_cast<int8_t>(std::min<uint32_t>(col->num_components(), 4u));
            col->ConvertValue<float>(col->mapped_index(pi), nc, c);
            out.colors[(i * 3) + 0] = c[0];
            out.colors[(i * 3) + 1] = c[1];
            out.colors[(i * 3) + 2] = c[2];
            if (col_alpha)
            {
                out.colors_alpha[i] = c[3];
            }
        }
    }

    out.indices.resize(nf * 3);
    for (size_t f = 0; f < nf; f++)
    {
        const draco::Mesh::Face &face = mesh->face(draco::FaceIndex(static_cast<uint32_t>(f)));
        out.indices[(f * 3) + 0] = face[0].value();
        out.indices[(f * 3) + 1] = face[1].value();
        out.indices[(f * 3) + 2] = face[2].value();
    }
    return true;
}
