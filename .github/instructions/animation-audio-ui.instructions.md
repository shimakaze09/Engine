---
description: "Use when editing audio playback, and future animation or UI runtime code. Covers audio safety, determinism, and authoring workflow rules."
name: "Animation Audio UI Rules"
applyTo: "animation/**, audio/**, runtime/**, editor/**, scripting/**"
---
# Animation Audio UI Rules

Every rule below is a hard gate. Violations are blocking defects.

## Audio Rules

- Audio initialization and device selection happen once at startup. Never mid-frame.
- Sound slot maximum is 256. Reject play requests beyond the cap with a log message and return false.
- Volume, pitch, and loop state changes take effect on the next audio callback. Never stall the main thread waiting for audio.
- All audio file loading goes through the VFS. Never use raw file paths.
- Audio error paths (device failure, file not found, format error) must log and degrade. Never crash.
- Audio state is owned by the audio module. Other modules request playback through the public API only.

## Authoring Workflow Rules

These apply to ALL gameplay-facing systems (audio, animation, UI, particles, etc.):

- Every runtime feature must expose hooks for editors and Lua scripts.
- Non-programmers must iterate on content without touching C++.
- Data-driven configuration (JSON, Lua tables, editor UI) over C++ hardcoded values.
- If a game designer cannot use the feature without a programmer's help, the API is wrong.

## Animation Rules (Apply when animation system is implemented)

- Pose evaluation must be deterministic under fixed timestep.
- Separate hot pose evaluation from editor-only metadata work.
- Skeleton data: SOA bone arrays, not AOS. Cache-friendly layout.
- GPU skinning via compute or vertex shader. Never CPU-side skinning.
- State machine transitions must be configurable by non-programmers via editor.

## UI Rules (Apply when UI system is implemented)

- Layout: invalidation-driven dirty flags. Never full rebuild every frame.
- Localization and text shaping are first-class, not afterthoughts.
- Keyboard/gamepad navigation must work without a mouse.
- Resolution and DPI scaling must preserve usable interaction targets.
