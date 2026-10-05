# 0022 — The engine patches bgfx locally, at configure time

**Date:** 2026-10-04. Decided while fixing #1223.

## Context

bgfx is fetched at a pinned commit (`CMakeLists.txt`, checked by
`tools/check_dependency_pins.py`). Its Direct3D 12 backend creates the
command queue's fence with the value 0 and also numbers the first
command list 0. `kick()` signals that number and arms the list's
completion event on it. The fence already holds 0, so the event fires at
once and the process's first command list counts as finished as soon as
it is submitted. Its upload buffers are released before the GPU has run
its copies, so the GPU can read freed memory.

This was first taken for the cause of #1223, where WARP loses one early
frame's GPU work. It is not that cause: with the fix applied, the Windows
Release lane still failed
`engine_integration_shadow_cache_far_cascade_gpu_after_boot_unchanged`
(job 111535390413). The fix stays because the early release is a defect
on its own.

There were three options for carrying a fix like this:

1. **Work around it in the engine.** This leaves the defect in place for
   everything the engine does not know to guard.
2. **Fork bgfx.** This fixes the cause, but a fork has to be rebased by
   hand on every bgfx update.
3. **Patch the fetched source.** This fixes the cause at its owner and
   keeps the upstream pin. Godot carries its third-party fixes the same
   way, as patches beside its `thirdparty/` sources, and re-applies them
   on every update.

## Decision

1. **The engine patches bgfx at configure time.** `cmake/patch_bgfx.cmake`
   runs after bgfx is fetched and before it compiles. Each fix is a
   literal replacement:
   - an already-patched tree is left alone, so a re-configure does
     nothing;
   - a tree that holds neither the original nor the patched text stops
     the configure, so a bgfx update cannot drop a fix silently.
2. **The first fix starts the Direct3D 12 command queue's count at 1.**
   Fence value 0 then means only "nothing to wait for".
3. **A fix is dropped when upstream carries it.** When a bgfx update
   fails the configure on a fix, read the new code. If upstream fixed
   the cause, delete the fix. Otherwise rewrite the fix's text for the
   new code.

## Consequences

- The pinned commit is unchanged, so the dependency pin gate still holds.
- Every local bgfx change lives in one file, each with its reason beside
  it.
- A patched tree is what the build compiles. Reading
  `build/_deps/bgfx-src` shows the patched code, not upstream's.
