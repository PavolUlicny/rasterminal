#pragma once

#include "src/args.h"
#include "src/render/camera.h"
#include "src/viewer/state.h"

namespace viewer
{

    // The viewer state an image depends on. The main loop configures the renderer from this
    // alone and redraws only when it differs from the last rendered one.
    struct FrameView
    {
        Camera camera;
        Background background;
        ShadingMode shading;
        LightingMode lighting;
        WireframeColor wireframe_color;
        bool culling;
        bool texturing;

        explicit FrameView(const ViewerState &state)
            : camera(state.camera), background(state.settings.background), shading(state.settings.shading),
              lighting(state.settings.lighting), wireframe_color(state.settings.wireframe_color),
              culling(state.settings.culling), texturing(state.settings.texturing)
        {
        }
    };

    // Wireframe draws edges in one colour without lights or textures, and the shaded
    // modes never read the edge colour, so a field the mode ignores cannot force a redraw.
    [[nodiscard]] inline bool same_image(const FrameView &a, const FrameView &b) noexcept
    {
        if (a.shading != b.shading || a.background != b.background || a.culling != b.culling ||
            !a.camera.same_view(b.camera))
        {
            return false;
        }
        if (a.shading == ShadingMode::Wireframe)
        {
            return a.wireframe_color == b.wireframe_color;
        }
        return a.lighting == b.lighting && a.texturing == b.texturing;
    }

} // namespace viewer
