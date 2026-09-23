# 0001 — bgfx is the rendering backend

**Date:** 2026-07-19. Backend flipped to default and the legacy GL backend
deleted at parity 2026-08-22.

## Context

The engine needs web export, which the hand-written OpenGL backend could
not deliver, and Apple's OpenGL cap made macOS a dead end for it. The
command-buffer frontend — builder, draw-key sort, render prep — was
designed to survive a backend swap, and the flush/init decomposition had
made the pass list an enumerable port surface.

## Decision

Adopt bgfx behind the engine-owned `RenderDevice` contract. `bgfx` is the
only accepted value of `ENGINE_RENDERER_BACKEND`; the cache variable
survives so existing `-D` invocations keep working.

The legacy GL backend was deleted once the bgfx path reached parity rather
than kept as a fallback: two backends meant every renderer change cost
twice and the second one was never verified.

## Consequences

- bgfx concepts must not appear above the backend translation unit. The
  device contract is the boundary.
  *(One sanctioned exception since: the editor's ImGui renderer,
  `editor/src/imgui_impl_bgfx.cpp`, calls bgfx directly, as `CLAUDE.md`
  records.)*
- Programs link only from cooked shader binaries; runtime GLSL compilation
  is unavailable. spirv is the canonical introspection profile.
- bgfx brings pipeline caching, so the GL shader binary cache stayed cut.
- Web export and the Metal path become reachable; both remain unproven
  until a lane demonstrates them.
