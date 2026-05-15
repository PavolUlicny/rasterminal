# Vendored libraries

All libraries are single-header (or minimal) and vendored directly. Do not edit these files manually — they are formatting-disabled via `vendor/.clang-format` and diff-suppressed in `.gitattributes`. To update a library, follow the refresh recipe below.

| Library | Version | Commit | Upstream | License |
| --- | --- | --- | --- | --- |
| stb_image | v2.30 | `31c1ad37456438565541f4919958214b6e762fb4` | <https://github.com/nothings/stb> | MIT / Unlicense (dual) |
| cgltf | v1.15 | `bbeb5b0b070ddacddac6852fb72143eb68454937` | <https://github.com/jkuhlmann/cgltf> | MIT |
| tinyply | 3.0 | `c9bb690dfe5e9105961e9e28120c48c9ae084bc6` | <https://github.com/ddiakopoulos/tinyply> | public domain |
| tinyobjloader | v2.0.0rc13 | `2945a967c5303b2c8c14174117c45f3302591150` | <https://github.com/tinyobjloader/tinyobjloader> | MIT |
| stl_reader | v2.0 | `a130fe0b2ac15d7c2fd642bf1dcbdec600e69151` | <https://github.com/sreiter/stl_reader> | BSD-2-Clause |

## Refresh recipe

```sh
# Example: update cgltf to v1.16
curl -sL https://raw.githubusercontent.com/jkuhlmann/cgltf/v1.16/cgltf.h -o vendor/cgltf/cgltf.h
curl -sL https://raw.githubusercontent.com/jkuhlmann/cgltf/v1.16/LICENSE  -o vendor/cgltf/LICENSE
git ls-remote https://github.com/jkuhlmann/cgltf refs/tags/v1.16
# Update the commit and version in this table, update THIRD_PARTY_NOTICES if the license changed, then test: make clean && make && make test
```

For stb (no per-file tags), use `master` and record the resolved HEAD SHA:

```sh
curl -sL https://raw.githubusercontent.com/nothings/stb/master/stb_image.h -o vendor/stb/stb_image.h
curl -sL https://raw.githubusercontent.com/nothings/stb/master/LICENSE      -o vendor/stb/LICENSE
git ls-remote https://github.com/nothings/stb HEAD
```

For stl_reader (header lives at `include/stl_reader/stl_reader.h` in the repo, but we flatten to `vendor/stl_reader/stl_reader.h`):

```sh
curl -sL https://raw.githubusercontent.com/sreiter/stl_reader/<tag>/include/stl_reader/stl_reader.h -o vendor/stl_reader/stl_reader.h
curl -sL https://raw.githubusercontent.com/sreiter/stl_reader/<tag>/LICENSE -o vendor/stl_reader/LICENSE
```
