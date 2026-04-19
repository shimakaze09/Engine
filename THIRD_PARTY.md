# Third-Party Dependencies

All dependencies are fetched via CMake FetchContent and pinned to exact SHA-1 commits.

| Library | Version | SHA-1 | License | Upstream |
|---------|---------|-------|---------|----------|
| SDL2 | 2.30.11 | `fa24d868ac2f8fd558e4e914c9863411245db8fd` | Zlib | https://github.com/libsdl-org/SDL |
| Lua | 5.4.6 | `6443185167c77adcc8552a3fee7edab7895db1a9` | MIT | https://github.com/lua/lua |
| Dear ImGui | docking branch | `329c5a6b3be75ebf54506d3ae94b836ffcf19fa0` | MIT | https://github.com/ocornut/imgui |
| ImGuizmo | master | `a15acd87a3f3241a29ea1363ceafc680dca3a96b` | MIT | https://github.com/CedricGuillemet/ImGuizmo |
| cgltf | 1.14 | `52c23814dbb60c2fbb54750ddf41d342d432a498` | MIT | https://github.com/jkuhlmann/cgltf |
| stb | master | `31c1ad37456438565541f4919958214b6e762fb4` | MIT/Public Domain | https://github.com/nothings/stb |
| miniaudio | 0.11.21 | header-only, vendored in `audio/src/` | MIT-0 | https://github.com/mackron/miniaudio |

## Update Procedure

1. Look up the new commit SHA on the upstream repository.
2. Update the `GIT_TAG` in `CMakeLists.txt`.
3. Update this table.
4. Run full build + test suite.
