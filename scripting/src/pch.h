// scripting/src/pch.h — Precompiled header for the engine_scripting module.
//
// Include only stable, high-frequency headers common to the majority of
// scripting translation units. The Lua C API is this module's private
// implementation domain (12 of 18 TUs include it); it must still never
// appear in scripting's public headers (module boundary rule).
//
// Usage: wired via engine_add_module_library(... PCH src/pch.h ...).

#pragma once

// ---- C standard library ----
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// ---- Lua C API (private to this module; never in public headers) ----
extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

// ---- Core engine logging (used by most scripting TUs) ----
#include "engine/core/logging.h"
