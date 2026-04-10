// core/src/pch.h — Precompiled header for the engine_core module.
//
// Include only stable, rarely-changing headers here.
// Do NOT include SDL, OpenGL, Lua, or ImGui types — they must not leak
// through core's public interface (module boundary rule).
//
// Usage: CMake target_precompile_headers(engine_core PRIVATE src/pch.h)

#pragma once

// ---- C standard library (stable, high-inclusion headers) ----
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

// ---- Core engine logging (used by almost every translation unit) ----
#include "engine/core/logging.h"
