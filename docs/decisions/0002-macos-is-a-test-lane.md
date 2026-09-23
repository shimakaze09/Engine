# 0002 — macOS is a build and headless-test lane

**Date:** 2026-08-09.

**Status:** The editor-target and shipping clauses are superseded by
[0015](0015-commercial-anime-engine-on-six-platforms.md); the AppleClang
conformance clause stands, now at AppleClang 16 (Xcode 16, the `macos-15`
CI image), which also compiles shaderc's tint: the macOS Release lane
cooks shaders, the `metal` profile included (#672).

## Context

macOS was originally blocked as a renderer target by Apple's OpenGL
version cap. With the bgfx migration that specific blocker is gone, but
maintaining a third editor platform costs review and verification capacity
the project does not have.

## Decision

macOS is a build and headless-test lane only. Its remaining value is
AppleClang conformance: it is the laggard on C++23 language features, so it
keeps the codebase honest about what actually compiles everywhere.

## Consequences

- No macOS editor target, and no GPU verification on macOS.
- Language features must compile under AppleClang; it gates the standard
  feature set the whole project may use.
- The lane never consumes cooked shader binaries, so it builds with the
  shader cook off.
- A macOS editor remains likely later. macOS game shipping stays a
  non-goal, and the iOS/Metal runtime proof needs hardware the project does
  not currently have.
