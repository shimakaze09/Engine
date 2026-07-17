// physics/src/pch.h — Precompiled header for the engine_physics module.
//
// Include only stable, high-frequency headers common to the majority of
// physics translation units. Math is header-only and every TU consumes
// Vec3; <cmath> appears in 11 of 12 TUs. Runtime headers must not appear
// here — physics talks to the world through PhysicsWorldView only.
//
// Usage: wired via engine_add_module_library(... PCH src/pch.h ...).

#pragma once

// ---- C standard library ----
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

// ---- Header-only math used by every physics TU ----
#include "engine/math/vec3.h"
