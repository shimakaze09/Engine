// Implements private Lua profiler, debugger, and hook-state bindings.

#include "debug_bindings.h"

#include "dap_server_internal.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace engine::scripting {
namespace {

constexpr std::size_t kMaxProfilerEntries = 256U;

/// Stores profiler sample counts by Lua function name.
struct ProfilerEntry final {
  char name[96] = {};
  std::uint32_t samples = 0U;
  bool occupied = false;
};

constexpr std::size_t kMaxBreakpoints = 64U;

/// Stores one source-line breakpoint.
struct DebugBreakpoint final {
  char file[160] = {};
  int line = 0;
  bool active = false;
};

constexpr std::size_t kMaxDebugWatches = 32U;
constexpr int kDefaultInstructionLimit = 1000000;

lua_State *g_hookState = nullptr;
ProfilerEntry g_profilerEntries[kMaxProfilerEntries]{};
bool g_profilerEnabled = false;
DebugBreakpoint g_breakpoints[kMaxBreakpoints]{};
char g_watchExprs[kMaxDebugWatches][96]{};
std::size_t g_watchCount = 0U;
char g_lastWatchOutput[1024] = {};
char g_lastCallstack[2048] = {};
char g_lastBreakpointFile[160] = {};
int g_lastBreakpointLine = 0;
std::uint32_t g_breakpointHitCount = 0U;
bool g_debuggerEnabled = false;
DapStepMode g_dapStepMode = DapStepMode::Continue;
int g_dapStepDepth = 0;
bool g_sandboxEnabled = true;
int g_instructionLimit = kDefaultInstructionLimit;

/// Records one profiler sample for the requested function name.
void profiler_record_sample(const char *name) noexcept {
  if ((name == nullptr) || (name[0] == '\0')) {
    name = "<anonymous>";
  }

  for (std::size_t i = 0U; i < kMaxProfilerEntries; ++i) {
    if (!g_profilerEntries[i].occupied) {
      continue;
    }
    if (std::strcmp(g_profilerEntries[i].name, name) == 0) {
      ++g_profilerEntries[i].samples;
      return;
    }
  }

  for (std::size_t i = 0U; i < kMaxProfilerEntries; ++i) {
    if (g_profilerEntries[i].occupied) {
      continue;
    }
    std::snprintf(g_profilerEntries[i].name, sizeof(g_profilerEntries[i].name),
                  "%s", name);
    g_profilerEntries[i].samples = 1U;
    g_profilerEntries[i].occupied = true;
    return;
  }
}

/// Returns whether a source/line location matches a registered breakpoint.
bool debugger_line_matches(const char *source, int line) noexcept {
  if ((source == nullptr) || (line <= 0)) {
    return false;
  }

  const char *normalizedSource = source;
  if (normalizedSource[0] == '@') {
    ++normalizedSource;
  }

  for (std::size_t i = 0U; i < kMaxBreakpoints; ++i) {
    if (!g_breakpoints[i].active || (g_breakpoints[i].line != line)) {
      continue;
    }
    const char *bp = g_breakpoints[i].file;
    const std::size_t srcLen = std::strlen(normalizedSource);
    const std::size_t bpLen = std::strlen(bp);
    if ((bpLen > 0U) && (srcLen >= bpLen) &&
        (std::strcmp(normalizedSource + (srcLen - bpLen), bp) == 0)) {
      return true;
    }
  }
  return false;
}

/// Captures configured debugger watch expression values.
void debugger_capture_watch_values(lua_State *state) noexcept {
  g_lastWatchOutput[0] = '\0';
  if ((state == nullptr) || (g_watchCount == 0U)) {
    return;
  }

  lua_sethook(state, nullptr, 0, 0);

  std::size_t writeOffset = 0U;
  for (std::size_t i = 0U; i < g_watchCount; ++i) {
    const char *expr = g_watchExprs[i];
    char chunk[160] = {};
    constexpr const char kWatchPrefix[] = "return (";
    constexpr std::size_t kWatchPrefixLength = sizeof(kWatchPrefix) - 1U;
    std::memcpy(chunk, kWatchPrefix, kWatchPrefixLength);
    std::size_t exprLength = 0U;
    while ((exprLength < (sizeof(g_watchExprs[0]) - 1U)) &&
           (expr[exprLength] != '\0')) {
      ++exprLength;
    }
    std::memcpy(chunk + kWatchPrefixLength, expr, exprLength);
    chunk[kWatchPrefixLength + exprLength] = ')';
    chunk[kWatchPrefixLength + exprLength + 1U] = '\0';

    const int loadStatus = luaL_loadstring(state, chunk);
    if (loadStatus != LUA_OK) {
      lua_pop(state, 1);
      continue;
    }

    const int callStatus = lua_pcall(state, 0, 1, 0);
    const char *value = "<error>";
    char valueBuffer[128] = {};
    if (callStatus == LUA_OK) {
      if (lua_isnumber(state, -1) != 0) {
        std::snprintf(valueBuffer, sizeof(valueBuffer), "%g",
                      static_cast<double>(lua_tonumber(state, -1)));
        value = valueBuffer;
      } else if (lua_isboolean(state, -1) != 0) {
        value = (lua_toboolean(state, -1) != 0) ? "true" : "false";
      } else if (lua_isstring(state, -1) != 0) {
        value = lua_tostring(state, -1);
      } else if (lua_isnil(state, -1) != 0) {
        value = "nil";
      } else {
        value = luaL_typename(state, -1);
      }
      lua_pop(state, 1);
    } else {
      lua_pop(state, 1);
    }

    if (writeOffset < (sizeof(g_lastWatchOutput) - 1U)) {
      const int written =
          std::snprintf(g_lastWatchOutput + writeOffset,
                        sizeof(g_lastWatchOutput) - writeOffset, "%s%s=%s",
                        (writeOffset == 0U) ? "" : "; ", expr, value);
      if (written > 0) {
        const std::size_t delta = static_cast<std::size_t>(written);
        writeOffset = (writeOffset + delta < sizeof(g_lastWatchOutput))
                          ? (writeOffset + delta)
                          : (sizeof(g_lastWatchOutput) - 1U);
      }
    }
  }

  refresh_debug_lua_hook();
}

/// Handles the Lua debug/profiler/sandbox hook callback.
void scripting_debug_hook(lua_State *state, lua_Debug *ar) noexcept {
  if ((state == nullptr) || (ar == nullptr)) {
    return;
  }

  if (g_sandboxEnabled && ar->event == LUA_HOOKCOUNT) {
    luaL_error(state, "CPU instruction limit exceeded (%d instructions)",
               g_instructionLimit);
    return;
  }

  if (g_profilerEnabled && (ar->event == LUA_HOOKCALL)) {
    if (lua_getinfo(state, "n", ar) != 0) {
      profiler_record_sample((ar->name != nullptr) ? ar->name : "<anonymous>");
    }
  }

  if (!g_debuggerEnabled) {
    return;
  }

  if ((g_dapStepMode == DapStepMode::StepOut) &&
      (ar->event == LUA_HOOKRET)) {
    lua_Debug check{};
    int depth = 0;
    while (lua_getstack(state, depth, &check) != 0) {
      ++depth;
    }
    if ((depth - 1) <= g_dapStepDepth) {
      g_dapStepMode = DapStepMode::StepIn;
    }
    return;
  }

  if (ar->event != LUA_HOOKLINE) {
    return;
  }

  if (lua_getinfo(state, "Sln", ar) == 0) {
    return;
  }

  bool shouldStop = false;
  const char *reason = "breakpoint";

  if (debugger_line_matches(ar->source, ar->currentline)) {
    shouldStop = true;
    reason = "breakpoint";
  } else if (g_dapStepMode == DapStepMode::StepIn) {
    shouldStop = true;
    reason = "step";
  } else if (g_dapStepMode == DapStepMode::Next) {
    lua_Debug check{};
    int depth = 0;
    while (lua_getstack(state, depth, &check) != 0) {
      ++depth;
    }
    if (depth <= g_dapStepDepth) {
      shouldStop = true;
      reason = "step";
    }
  }

  if (!shouldStop) {
    return;
  }

  const char *source = (ar->source != nullptr) ? ar->source : "";
  if (source[0] == '@') {
    ++source;
  }
  std::snprintf(g_lastBreakpointFile, sizeof(g_lastBreakpointFile), "%s",
                source);
  g_lastBreakpointLine = ar->currentline;
  ++g_breakpointHitCount;

  luaL_traceback(state, state, "breakpoint", 1);
  const char *trace = lua_tostring(state, -1);
  if (trace != nullptr) {
    std::snprintf(g_lastCallstack, sizeof(g_lastCallstack), "%s", trace);
  }
  lua_pop(state, 1);
  debugger_capture_watch_values(state);

  if (dap_has_client()) {
    lua_Debug depthCheck{};
    int currentDepth = 0;
    while (lua_getstack(state, currentDepth, &depthCheck) != 0) {
      ++currentDepth;
    }
    g_dapStepMode = dap_on_stopped(state, source, ar->currentline, reason);
    g_dapStepDepth = currentDepth;
    refresh_debug_lua_hook();
  }
}

} // namespace

int lua_engine_profiler_enable(lua_State *state) noexcept {
  g_profilerEnabled =
      (lua_gettop(state) >= 1) && (lua_toboolean(state, 1) != 0);
  refresh_debug_lua_hook();
  lua_pushboolean(state, 1);
  return 1;
}

int lua_engine_profiler_reset(lua_State *state) noexcept {
  static_cast<void>(state);
  for (std::size_t i = 0U; i < kMaxProfilerEntries; ++i) {
    g_profilerEntries[i] = ProfilerEntry{};
  }
  return 0;
}

int lua_engine_profiler_get_count(lua_State *state) noexcept {
  const char *name = lua_tostring(state, 1);
  if (name == nullptr) {
    lua_pushinteger(state, 0);
    return 1;
  }

  for (std::size_t i = 0U; i < kMaxProfilerEntries; ++i) {
    if (!g_profilerEntries[i].occupied) {
      continue;
    }
    if (std::strcmp(g_profilerEntries[i].name, name) == 0) {
      lua_pushinteger(state,
                      static_cast<lua_Integer>(g_profilerEntries[i].samples));
      return 1;
    }
  }

  lua_pushinteger(state, 0);
  return 1;
}

int lua_engine_debugger_enable(lua_State *state) noexcept {
  g_debuggerEnabled =
      (lua_gettop(state) >= 1) && (lua_toboolean(state, 1) != 0);
  refresh_debug_lua_hook();
  lua_pushboolean(state, 1);
  return 1;
}

int lua_engine_debugger_add_breakpoint(lua_State *state) noexcept {
  const char *file = lua_tostring(state, 1);
  if ((file == nullptr) || (lua_isnumber(state, 2) == 0)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const int line = static_cast<int>(lua_tointeger(state, 2));
  lua_pushboolean(state, debugger_add_breakpoint(file, line) ? 1 : 0);
  return 1;
}

int lua_engine_debugger_clear_breakpoints(lua_State *state) noexcept {
  static_cast<void>(state);
  debugger_clear_breakpoints();
  lua_pushboolean(state, 1);
  return 1;
}

int lua_engine_debugger_add_watch(lua_State *state) noexcept {
  const char *expr = lua_tostring(state, 1);
  if ((expr == nullptr) || (g_watchCount >= kMaxDebugWatches)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  std::snprintf(g_watchExprs[g_watchCount], sizeof(g_watchExprs[0]), "%s",
                expr);
  ++g_watchCount;
  lua_pushboolean(state, 1);
  return 1;
}

int lua_engine_debugger_clear_watches(lua_State *state) noexcept {
  static_cast<void>(state);
  g_watchCount = 0U;
  g_lastWatchOutput[0] = '\0';
  return 0;
}

int lua_engine_debugger_last_breakpoint(lua_State *state) noexcept {
  if (g_lastBreakpointLine <= 0) {
    lua_pushnil(state);
    return 1;
  }
  lua_newtable(state);
  lua_pushstring(state, g_lastBreakpointFile);
  lua_setfield(state, -2, "file");
  lua_pushinteger(state, static_cast<lua_Integer>(g_lastBreakpointLine));
  lua_setfield(state, -2, "line");
  lua_pushinteger(state, static_cast<lua_Integer>(g_breakpointHitCount));
  lua_setfield(state, -2, "hits");
  return 1;
}

int lua_engine_debugger_last_callstack(lua_State *state) noexcept {
  lua_pushstring(state, g_lastCallstack);
  return 1;
}

int lua_engine_debugger_last_watch_values(lua_State *state) noexcept {
  lua_pushstring(state, g_lastWatchOutput);
  return 1;
}

void set_debug_lua_state(lua_State *state) noexcept { g_hookState = state; }

void refresh_debug_lua_hook() noexcept {
  if (g_hookState == nullptr) {
    return;
  }

  int mask = 0;
  int count = 0;
  if (g_profilerEnabled) {
    mask |= LUA_MASKCALL;
  }
  if (g_debuggerEnabled) {
    mask |= LUA_MASKLINE;
    if (g_dapStepMode != DapStepMode::Continue) {
      mask |= LUA_MASKCALL | LUA_MASKRET;
    }
  }
  if (g_sandboxEnabled && g_instructionLimit > 0) {
    mask |= LUA_MASKCOUNT;
    count = g_instructionLimit;
  }

  if (mask == 0) {
    lua_sethook(g_hookState, nullptr, 0, 0);
    return;
  }

  lua_sethook(g_hookState, &scripting_debug_hook, mask, count);
}

void reset_debug_bindings() noexcept {
  for (std::size_t i = 0U; i < kMaxProfilerEntries; ++i) {
    g_profilerEntries[i] = ProfilerEntry{};
  }
  debugger_clear_breakpoints();
  g_watchCount = 0U;
  g_lastWatchOutput[0] = '\0';
  g_lastCallstack[0] = '\0';
  g_lastBreakpointFile[0] = '\0';
  g_lastBreakpointLine = 0;
  g_breakpointHitCount = 0U;
  g_profilerEnabled = false;
  g_debuggerEnabled = false;
  g_dapStepMode = DapStepMode::Continue;
  g_dapStepDepth = 0;
}

void debugger_clear_breakpoints() noexcept {
  for (std::size_t i = 0U; i < kMaxBreakpoints; ++i) {
    g_breakpoints[i] = DebugBreakpoint{};
  }
}

bool debugger_add_breakpoint(const char *file, int line) noexcept {
  if ((file == nullptr) || (line <= 0)) {
    return false;
  }
  for (std::size_t i = 0U; i < kMaxBreakpoints; ++i) {
    if (!g_breakpoints[i].active) {
      std::snprintf(g_breakpoints[i].file, sizeof(g_breakpoints[i].file), "%s",
                    file);
      g_breakpoints[i].line = line;
      g_breakpoints[i].active = true;
      return true;
    }
  }
  return false;
}

void set_debug_sandbox_enabled(bool enabled) noexcept {
  g_sandboxEnabled = enabled;
}

bool debug_sandbox_enabled() noexcept { return g_sandboxEnabled; }

void set_debug_instruction_limit(int limit) noexcept {
  g_instructionLimit = limit;
}

int debug_instruction_limit() noexcept { return g_instructionLimit; }

} // namespace engine::scripting
