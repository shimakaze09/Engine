---
description: "Use when editing renderer or editor systems, including GL context ownership, draw command flow, pass resource management, and Play/Stop behavior."
name: "Renderer and Editor Rules"
applyTo: "renderer/**, editor/**"
---
# Renderer and Editor Rules

Every rule below is a hard gate. Violations are blocking defects.

## Render Pipeline

Current pipeline (3 passes, forward-only):
1. **Scene pass**: PBR shader → HDR FBO (sceneColorTexture + sceneDepthTexture)
2. **Tonemap pass**: fullscreen triangle, reads sceneColor → LDR FBO (finalColorTexture)
3. **Back buffer**: clear for ImGui overlay

Rules:
- Render commands: fill → merge → sort → flush. No other ordering.
- `flush_renderer()` assumes the caller owns the GL context.
- Engine main loop owns render, editor, and swap ordering.
- Renderer and editor code must NOT acquire or release GL context.

## GL Function Routing

- All OpenGL calls go through the `RenderDevice` function pointer table.
- Never call raw `gl*` functions directly. No exceptions.
- This is the future RHI abstraction point. Do not bypass it.

## Pass Resource Contract

Passes communicate through `PassResourceId` handles, NOT raw texture or FBO IDs.

Adding a new render pass requires ALL of the following:

1. Add `PassResourceId` slot(s) in `pass_resources.h`.
2. Allocate in `create_gpu_resources()` — create order forward.
3. Destroy in `destroy_gpu_resources()` — destroy order reverse.
4. Handle resize in `resize_pass_resources()`.
5. Load shader in `initialize_backend()` with `reset_backend_on_failure()` on error.
6. Execute in `flush_renderer()` in correct pipeline position.
7. Restore GL state after pass: depth mask, blend, cull face.

Constraints:
- Existing pass code must not change when adding new passes.
- Resource lifetime and resize are centralized in `pass_resources.cpp`.
- Every GPU create has a matching destroy. No leaks on resize or shutdown.

## Shader Rules

- Minimum GLSL version: 330 core.
- Source files: `assets/shaders/`. No hardcoded GLSL strings.
- Scene shaders output linear HDR. Tone mapping is a separate post-process.
- New uniforms: add to backend state and initialize in backend setup.

## Editor Phase Gating

- All editor world mutations require `world_is_editable()` AND WorldPhase::Idle. Both.
- Inspector is read-only during play mode.
- Play/Stop flow:
  1. Play: snapshot world state → enter simulation.
  2. Pause: freeze simulation, allow editor reads only.
  3. Stop: restore snapshot → return to Idle.

## Culling

- Frustum culling: per-frame AABB vs planes in render prep.
- Culling is CPU-side only.
- No occlusion culling, no distance culling, no LOD selection exists yet.
