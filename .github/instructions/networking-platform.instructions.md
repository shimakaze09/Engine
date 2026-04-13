---
description: "Use when editing networking, platform abstraction, packaging, or OS integration code. Covers authority model, transport security, and platform gating invariants."
name: "Networking and Platform Rules"
applyTo: "app/**, runtime/**, tests/integration/**, tests/smoke/**"
---
# Networking and Platform Rules

These are invariants that apply the moment networking or platform code is written. They are not implementation guidance for unbuilt systems.

## Network Security Invariants

- Server is authoritative. Client is never trusted for game state. No exceptions.
- All packets must be authenticated. No plaintext game state on the wire.
- Validate packet sizes and bounds before deserialization. Buffer overflows in network code are critical vulnerabilities.
- Never trust client-supplied indices, IDs, or sizes without bounds checking.
- Rate-limit connection attempts. Implement connection throttling.
- Every replicated property must declare its authority: server-only, client-predicted, or autonomous proxy.

## Platform Gating

- Centralize platform abstraction. No scattered `#ifdef _WIN32` through engine code.
- Platform-specific code must go behind compile-time macros in `core/include/engine/core/platform.h` or runtime capability checks.
- Packaging and deployment must be reproducible and non-interactive in CI.
- File paths must handle platform differences: separators, case sensitivity, max length.

## OS Integration Safety

- Failures in native dialogs, clipboard, notifications, or file operations must degrade gracefully. Never crash.
- Display mode changes (fullscreen, borderless) must handle multi-monitor configurations.
- Accessibility features must not depend on optional rendering features.

## Graphics API Abstraction

- All OpenGL calls go through `RenderDevice` function pointer table.
- Never leak platform-specific API types (GL enums, VK handles, D3D pointers) into runtime or gameplay layers.
- Resource creation and destruction must be explicit and trackable.
