#pragma once

#include "camera.h"
#include "framebuffer.h"
#include "light.h"
#include "mesh.h"

enum class ShadingMode
{
    Wireframe,
    Flat,
    Gouraud,
    Phong
};

struct Renderer
{
    ShadingMode mode = ShadingMode::Gouraud;

    void render(const Mesh &, const Camera &, const Light &, Framebuffer &) const;

    // Cycle: Wireframe → Flat → Gouraud → Phong → Wireframe
    void cycle_shading();
};
