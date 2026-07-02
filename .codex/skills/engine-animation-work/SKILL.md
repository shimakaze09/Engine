---
name: engine-animation-work
description: Implement or plan the Engine repository's skeletal animation tasks, including glTF skeleton and clip loading, compressed animation data, pose blending, blend spaces, animation state machines, root motion, animation events, skinned mesh rendering, GPU skinning shaders, and IK. Use when requests mention P1-M7, animation systems, skeletons, clips, skinning, animation state, root motion, notifies, or animation editor dependencies.
---

# Engine Animation Work

## Workflow

Start in `D:\dev\Engine` unless the user points elsewhere.

Read first:

- `AGENTS.md`
- `README.md`
- `TODO.md`
- Root `CMakeLists.txt`
- `runtime/CMakeLists.txt`, `renderer/CMakeLists.txt`, `tools/CMakeLists.txt`, and `tests/CMakeLists.txt`
- Renderer mesh/material/shader headers and tests
- Runtime world/component, scene serialization, prefab serialization, and transform tests
- `tools/asset_packer` code before changing glTF import or cooking
- Shader assets under `assets/shaders/` before adding skinning variants

Run `git status --short` before edits and preserve unrelated user changes.

## Architecture Rules

Keep animation data ownership explicit:

- Runtime owns animation components, playback state, root motion application, and Lua-facing state queries.
- Renderer owns skinned draw submission, shader variants, bone-palette upload, and GL execution details.
- Tools or asset code owns glTF skin/animation parsing, compression, metadata, and deterministic cooked output.
- Scripting binds a narrow API for parameters/events; do not leak Lua types into runtime public headers.
- Public headers must stay self-contained and free of SDL, OpenGL, Lua, ImGui, and ImGuizmo types.

## Build Order

Prefer this sequence:

1. Add data types for `Skeleton`, `AnimClip`, compressed tracks, and animation asset handles.
2. Extend glTF import/cooking for skins and clips with deterministic tests.
3. Add pose sampling and blend operations with math-only unit tests.
4. Add playback component/state machine runtime and serialization coverage.
5. Add root motion and event dispatch with ECS and scripting bridge tests.
6. Add renderer skinning path and shader variants with CPU-verifiable command/shader tests.
7. Add IK only after clip playback and skinned rendering are stable.

## Verification

Add tests for:

- Clip decompression error bounds and deterministic cooking.
- Pose interpolation, additive/masked blends, and 1D/2D blend-space weights.
- State transitions, triggers, crossfade timing, and hot reload where applicable.
- Root motion interaction with transforms and physics handoff.
- Animation events firing exactly once across frame boundaries.
- Skinned command generation and shader variant selection without requiring GPU-only assertions when possible.

Avoid per-frame heap allocation in sampling, blending, state-machine update, and render-prep paths.
