#pragma once

#include "src/args.h"
#include "src/render/camera.h"

namespace viewer
{

    struct ViewerSettings
    {
        ShadingMode shading;
        Background background;
        LightingMode lighting;
        WireframeColor wireframe_color;
        bool spinning;
        bool culling;
        bool texturing;

        explicit ViewerSettings(const ParsedArgs &args)
            : shading(args.shading), background(args.bg), lighting(args.lighting),
              wireframe_color(args.wireframe_color), spinning(args.spin), culling(args.cull), texturing(args.texture)
        {
        }
    };

    struct ViewerState
    {
        Camera camera;
        ViewerSettings settings;

        ViewerState(const ParsedArgs &args, const Camera &launch_camera)
            : camera(launch_camera), settings(args), launch_camera_(launch_camera), launch_settings_(args)
        {
        }

        void reset()
        {
            camera = launch_camera_;
            settings = launch_settings_;
        }

      private:
        Camera launch_camera_;
        ViewerSettings launch_settings_;
    };

} // namespace viewer
