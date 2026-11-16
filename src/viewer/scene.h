#pragma once

#include "src/args.h"
#include "src/loaders/mesh.h"
#include "src/math/light.h"
#include "src/math/linalg.h"
#include "src/render/camera.h"

#include <algorithm>

namespace viewer
{
    inline constexpr vec3 FLAT_AMBIENT = { 0.85f, 0.85f, 0.85f };

    // Directional lights active for a lighting mode; Flat is ambient-only.
    constexpr int light_count(LightingMode m) noexcept
    {
        switch (m)
        {
        case LightingMode::Single:
            return 1;
        case LightingMode::Flat:
            return 0;
        case LightingMode::Dual:
            return 2;
        }
        return 2;
    }

    // Flat mode swaps the scene ambient for the bright uniform FLAT_AMBIENT so the
    // full model stays visible with no directional lights.
    constexpr vec3 lighting_ambient(LightingMode m, const vec3 &ambient) noexcept
    {
        return m == LightingMode::Flat ? FLAT_AMBIENT : ambient;
    }

    struct SceneLights
    {
        Light lights[2];
        vec3 ambient;
    };

    // The key light is warm white from the upper right front. The fill light is dim
    // cool blue from the lower left back, for contrast.
    inline SceneLights default_lights() noexcept
    {
        SceneLights scene;
        scene.lights[0].direction = { 0.408f, 0.816f, 0.408f };
        scene.lights[0].color = { 0.85f, 0.77f, 0.64f };
        scene.lights[1].direction = { -0.667f, -0.333f, -0.667f };
        scene.lights[1].color = { 0.3f, 0.3f, 0.33f };
        scene.ambient = { 0.32f, 0.32f, 0.33f };
        return scene;
    }

    // The bench passes false because its spinning camera is a turntable operation;
    // first-person would pan the model out of frame and clamp the requested pitch.
    inline Camera auto_fit_camera(const Mesh &mesh, const ParsedArgs &args, bool first_person)
    {
        vec3 lo = mesh.vertices[0].pos;
        vec3 hi = lo;
        for (const Vertex &v : mesh.vertices)
        {
            lo.x = std::min(lo.x, v.pos.x);
            lo.y = std::min(lo.y, v.pos.y);
            lo.z = std::min(lo.z, v.pos.z);
            hi.x = std::max(hi.x, v.pos.x);
            hi.y = std::max(hi.y, v.pos.y);
            hi.z = std::max(hi.z, v.pos.z);
        }
        const vec3 centre = (lo + hi) * 0.5f;
        float radius = (hi - lo).length() * 0.5f;
        if (radius < 1e-4f)
        {
            radius = 1.0f; // degenerate (all coincident vertices): use sane defaults
        }

        Camera camera;
        camera.first_person = first_person;
        camera.fp_centre = centre;
        // Scale movement by bounding radius so multiplier 1 crosses the model in about
        // two seconds. Long thin models therefore use their long axis.
        camera.fp_base_speed = radius;
        camera.fp_speed = args.first_person_speed;
        camera.target = centre;
        // --zoom's parse-time bound [0.2, 100] lands the distance inside the
        // interactive clamp [near*2, far*0.5] by construction, so no clamp here.
        camera.distance = radius * 2.0f / args.zoom;
        // Scale clipping planes by model size. First-person may fly through the near
        // plane; shrinking it would reduce depth precision for the whole scene.
        camera.near_plane = radius * 0.01f;
        camera.far_plane = radius * 20.0f;
        // Build the launch pose through orbit(), matching drag composition and the
        // first-person target invariant. Negate yaw for orbit's screen-drag convention.
        // First-person uses the same pitch clamp as look().
        const float pitch_rad = first_person
                                    ? clamp(to_radians(args.pitch), -Camera::FP_MAX_PITCH, Camera::FP_MAX_PITCH)
                                    : to_radians(args.pitch);
        camera.orbit(-to_radians(args.yaw), pitch_rad);
        return camera;
    }
} // namespace viewer
