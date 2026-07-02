---
name: engine-ui-runtime
description: Implement or plan the Engine repository's game runtime UI system, including canvas rendering, resolution-independent layout, 2D batching, font/SDF text, widgets, input routing, focus/gamepad navigation, UI tweens, localization hooks, accessibility settings, and Lua UI bindings. Use when requests mention P1-M11, runtime UI, HUD, menus, widgets, canvas, font rendering, UI layout, UI input, or game-facing UI APIs.
---

# Engine UI Runtime

## Workflow

Start in `D:\dev\Engine` unless the user points elsewhere.

Read first:

- `AGENTS.md`
- `README.md`
- `TODO.md`
- Root `CMakeLists.txt`
- `runtime/CMakeLists.txt`, `renderer/CMakeLists.txt`, `scripting/CMakeLists.txt`, and `tests/CMakeLists.txt`
- Renderer command buffer, pass resource, shader system, and post-process tests
- Core input/input-map/touch tests
- Scripting bridge and Lua binding tests
- Existing editor code only to avoid duplicating problems; runtime UI must not depend on ImGui

Run `git status --short` before edits and preserve unrelated user changes.

## Architecture Rules

Keep game UI independent from editor UI:

- Do not require ImGui, ImGuizmo, or editor-only modules for runtime UI.
- Runtime should own UI tree/state and input routing; renderer should own draw command generation and backend execution.
- Scripting should expose stable Lua handles and property APIs without exposing Lua C API types outside scripting boundaries.
- Font, texture, and material assets should flow through renderer/asset systems rather than ad hoc global loaders.
- Public APIs must keep SDL/OpenGL/Lua/ImGui types out of engine-facing headers.

## Build Order

Prefer this sequence:

1. Add UI canvas coordinates, anchors, scale policy, and clipped rect math with unit tests.
2. Add batched textured-quad UI draw preparation and a post-tonemap UI pass.
3. Add font atlas/text layout, then SDF or scalable text once basic glyph rendering is covered.
4. Add widget primitives and layout containers with deterministic layout tests.
5. Add hit testing, focus routing, input consumption, and gamepad navigation.
6. Add tween timelines and callbacks after widget state is stable.
7. Add Lua API bindings, localization hooks, and accessibility settings.

## Verification

Add tests for:

- Layout at multiple logical and physical resolutions.
- Hit-test ordering, clipping, focus movement, modal consumption, and touch/gamepad routing.
- Widget state transitions and callback dispatch.
- Text measurement, glyph fallback behavior, and atlas cache limits where possible.
- Lua binding stack behavior and error paths.
- CPU-verifiable render-prep output before relying on GPU screenshots.

Avoid heap allocation in per-frame layout, input routing, tween update, and draw batching paths unless storage is explicitly preallocated.
