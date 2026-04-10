#pragma once

#include "camera.h"
#include "framebuffer.h"
#include "light.h"
#include "mesh.h"

enum class ShadingMode
{
    Wireframe,
    Flat,
    Gouraud
};

struct Renderer
{
    ShadingMode mode = ShadingMode::Gouraud;

    void render(const Mesh &, const Camera &, const Light &, Framebuffer &) const;

    // Cycle: Wireframe → Flat → Gouraud → Wireframe
    void cycle_shading();
};
