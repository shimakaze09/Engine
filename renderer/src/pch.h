// renderer/src/pch.h — Precompiled header for the engine_renderer module.
//
// Include only stable, high-frequency headers common to the majority of
// renderer translation units. Engine module boundary rules apply:
// - Do NOT include SDL, OpenGL, or ImGui types in PRIVATE renderer headers.
// - GL headers remain in render_device_gl.cpp where they belong.
//
// Usage: CMake target_precompile_headers(engine_renderer PRIVATE src/pch.h)

#pragma once

// ---- C standard library ----
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// ---- Core engine utilities (used by almost every renderer TU) ----
#include "engine/core/logging.h"
#include "engine/core/platform.h"
