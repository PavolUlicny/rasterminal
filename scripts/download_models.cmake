# Download third-party test models that are not redistributed in the repo.
# Run from the repo root: cmake -P scripts/download_models.cmake
#
# Sources:
#   Duck (gltf + glb)   — KhronosGroup/glTF-Sample-Assets (MIT / SCEA SSL v1.0)
#                         https://github.com/KhronosGroup/glTF-Sample-Assets
#   Teapot, Suzanne,
#   XYZ RGB Dragon      — alecjacobson/common-3d-test-models
#                         https://github.com/alecjacobson/common-3d-test-models
#   Stanford Bunny      — reprap-io/reprapio_stanford_bunny
#                         https://github.com/reprap-io/reprapio_stanford_bunny
#                         Original: Stanford Computer Graphics Laboratory

cmake_minimum_required(VERSION 3.15)

set(BASE_KHRONOS "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/Duck")
set(BASE_ALEC    "https://raw.githubusercontent.com/alecjacobson/common-3d-test-models/master/data")
set(BASE_BUNNY   "https://raw.githubusercontent.com/reprap-io/reprapio_stanford_bunny/master")

set(MODELS
    "models/gltf/Duck.gltf|${BASE_KHRONOS}/glTF/Duck.gltf"
    "models/gltf/Duck0.bin|${BASE_KHRONOS}/glTF/Duck0.bin"
    "models/gltf/DuckCM.png|${BASE_KHRONOS}/glTF/DuckCM.png"
    "models/glb/Duck.glb|${BASE_KHRONOS}/glTF-Binary/Duck.glb"
    "models/obj/teapot.obj|${BASE_ALEC}/teapot.obj"
    "models/obj/suzanne.obj|${BASE_ALEC}/suzanne.obj"
    "models/obj/xyzrgb_dragon.obj|${BASE_ALEC}/xyzrgb_dragon.obj"
    "models/stl/Stanford_Bunny.stl|${BASE_BUNNY}/bunny.stl"
)

foreach(entry IN LISTS MODELS)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 dest)
    list(GET parts 1 url)

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${dest}")
        message(STATUS "Already exists: ${dest}")
        continue()
    endif()

    message(STATUS "Downloading ${dest} ...")
    file(DOWNLOAD "${url}" "${CMAKE_CURRENT_SOURCE_DIR}/${dest}"
        SHOW_PROGRESS
        STATUS dl_status
    )
    list(GET dl_status 0 dl_code)
    list(GET dl_status 1 dl_error)
    if(NOT dl_code EQUAL 0)
        message(FATAL_ERROR "Failed to download ${dest}: ${dl_error}")
    endif()
endforeach()

message(STATUS "All models ready.")
