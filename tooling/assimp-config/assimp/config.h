#pragma once

#ifndef AI_CONFIG_H_INC
#define AI_CONFIG_H_INC

// CMake generates this header under the build directory. This small, project-owned
// equivalent lets compile_flags.txt parse Assimp headers before a build exists. Keep
// it in sync with the CMake configuration: Assimp's defaults, plus the AI_CONFIG_*
// keys mesh_assimp.cpp sets.
#define AI_CONFIG_CHECK_IDENTITY_MATRIX_EPSILON_DEFAULT 10e-3f
#define AI_CONFIG_PP_GSN_MAX_SMOOTHING_ANGLE "PP_GSN_MAX_SMOOTHING_ANGLE"
#define AI_CONFIG_IMPORT_NO_SKELETON_MESHES "IMPORT_NO_SKELETON_MESHES"
#define AI_CONFIG_IMPORT_MD5_NO_ANIM_AUTOLOAD "IMPORT_MD5_NO_ANIM_AUTOLOAD"
#define AI_CONFIG_IMPORT_MDL_COLORMAP "IMPORT_MDL_COLORMAP"
#define AI_CONFIG_IMPORT_UNREAL_HANDLE_FLAGS "UNREAL_HANDLE_FLAGS"
#define AI_CONFIG_IMPORT_ASE_RECONSTRUCT_NORMALS "IMPORT_ASE_RECONSTRUCT_NORMALS"

#endif
