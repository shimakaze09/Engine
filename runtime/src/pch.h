// runtime/src/pch.h — Precompiled header for the engine_runtime module.
//
// Include only stable, high-frequency headers common to the majority of
// runtime translation units. world.h dominates runtime compile time and
// 9 of 15 TUs include it, so it is precompiled here deliberately: editing
// world.h already rebuilt most of the module before the PCH existed.
//
// Usage: wired via engine_add_module_library(... PCH src/pch.h ...).

#pragma once

// ---- C standard library ----
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// ---- Engine headers used by most runtime TUs ----
#include "engine/core/logging.h"
#include "engine/runtime/world.h"
