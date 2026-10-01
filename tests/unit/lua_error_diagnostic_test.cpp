// Pins that a Lua error reaches the diagnostic sinks as a record naming
// the script and line Lua reported, through the production load and
// error-logging path, so the editor console navigates without parsing
// the message text.

#include <cstdio>
#include <cstring>

#include "../test_harness.h"
#include "engine/core/diagnostic.h"
#include "engine/core/logging.h"
#include "engine/scripting/scripting.h"

namespace {

constexpr const char *kScriptPath = "lua_error_diagnostic_test.lua";

engine::core::Diagnostic g_last{};
int g_scriptingRecords = 0;

void capture(const engine::core::Diagnostic &record, void *) noexcept {
  if (std::strcmp(record.channel, "scripting") == 0) {
    g_last = record;
    ++g_scriptingRecords;
  }
}

bool write_script(const char *code) noexcept {
  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, kScriptPath, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(kScriptPath, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t length = std::strlen(code);
  const bool ok = (std::fwrite(code, 1U, length, file) == length);
  std::fclose(file);
  return ok;
}

} // namespace

/// Runs this executable or test program.
int main() {
  engine::tests::TestContext ctx;
  ctx.check(engine::core::initialize_logging(), "initialize logging");
  ctx.check(engine::core::log_register_diagnostic_sink(&capture, nullptr),
            "register the record sink");
  ctx.check(engine::scripting::initialize_scripting(), "initialize scripting");

  // A runtime error on the third line: Lua's own "<chunk>:<line>:" prefix
  // is what the record's path and line are read from.
  ctx.check(write_script("local x = 1\nlocal y = 2\nerror('boom')\n"),
            "write the failing script");
  g_scriptingRecords = 0;
  ctx.check(!engine::scripting::load_script(kScriptPath),
            "the failing script fails to load");
  ctx.check(g_scriptingRecords >= 1, "the failure reached the record sink");
  const std::size_t pathLength = std::strlen(g_last.path);
  const std::size_t nameLength = std::strlen(kScriptPath);
  ctx.check((pathLength >= nameLength) &&
                (std::strcmp(g_last.path + (pathLength - nameLength),
                             kScriptPath) == 0),
            "the record names the script");
  ctx.check(g_last.line == 3, "the record carries Lua's line");
  ctx.check(g_last.level == engine::core::LogLevel::Error, "it is an error");
  ctx.check(std::strstr(g_last.message, "boom") != nullptr,
            "the message text is the Lua error");

  // An error two calls deep logs the call chain that reached it. The
  // traceback used to be built after lua_pcall had unwound the stack, so
  // it described only the logger's own call and named no script frame.
  ctx.check(write_script("local function inner()\n"
                         "  error('deep')\n"
                         "end\n"
                         "local function outer()\n"
                         "  inner()\n"
                         "end\n"
                         "outer()\n"),
            "write the nested failing script");
  g_scriptingRecords = 0;
  ctx.check(!engine::scripting::load_script(kScriptPath),
            "the nested failing script fails to load");
  std::printf("nested error record: %s\n", g_last.message);
  ctx.check((g_scriptingRecords >= 1) && (g_last.line == 2),
            "the record names the line that raised");
  ctx.check(std::strstr(g_last.message, "'inner'") != nullptr,
            "the traceback names the function that raised");
  ctx.check(std::strstr(g_last.message, "'outer'") != nullptr,
            "the traceback names its caller");
  ctx.check(std::strstr(g_last.message, "lua_error_diagnostic_test.lua:5:") !=
                nullptr,
            "the traceback gives the caller's line");

  // A coroutine that fails keeps its frames, so its error carries the
  // coroutine's own call chain, read from the coroutine.
  ctx.check(write_script("local function failing()\n"
                         "  error('in coroutine')\n"
                         "end\n"
                         "engine.start_coroutine(function()\n"
                         "  failing()\n"
                         "end)\n"),
            "write the failing coroutine script");
  g_scriptingRecords = 0;
  static_cast<void>(engine::scripting::load_script(kScriptPath));
  ctx.check((g_scriptingRecords >= 1) &&
                (std::strstr(g_last.message, "in coroutine") != nullptr) &&
                (std::strstr(g_last.message, "'failing'") != nullptr),
            "a coroutine's error names the function that raised in it");

  // A syntax error is reported the same way, on its own line.
  ctx.check(write_script("local ok = true\nthis is not lua\n"),
            "write the unparsable script");
  g_scriptingRecords = 0;
  ctx.check(!engine::scripting::load_script(kScriptPath),
            "the unparsable script fails to load");
  ctx.check((g_scriptingRecords >= 1) && (g_last.line == 2),
            "a syntax error names its line");

  engine::scripting::shutdown_scripting();
  engine::core::log_unregister_diagnostic_sink(&capture, nullptr);
  engine::core::shutdown_logging();
  static_cast<void>(std::remove(kScriptPath));
  return ctx.finish("lua_error_diagnostic");
}
