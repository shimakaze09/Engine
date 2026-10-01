// Lua's print reaches the log, which the editor's Log panel and log file
// show. It used to write to stdout, which an editor started as a GUI
// program has none of, so an author's first debug line appeared nowhere.
// print("a", 1, nil) logs one Info line naming the script and line, with
// its arguments joined by tabs as Lua's own print joins them; an empty
// print logs an empty line; a value's __tostring is honoured.

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "../test_harness.h"
#include "engine/core/logging.h"
#include "engine/core/service_locator.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace {

namespace sc = engine::scripting;
namespace rt = engine::runtime;

constexpr const char *kDriverPath = "lua_print_driver.lua";

constexpr const char *kDriver =
    "function print_values() print('a', 1, nil) end\n" // 1
    "function print_nothing() print() end\n"           // 2
    "local point = setmetatable({}, {__tostring = function() "
    "return 'point(1, 2)' end})\n"                      // 3
    "function print_object() print('at', point) end\n"; // 4

std::vector<std::string> g_lines{};

void capture(engine::core::LogLevel level, const char *channel,
             const char *message, void * /*userData*/) noexcept {
  if ((level == engine::core::LogLevel::Info) && (channel != nullptr) &&
      (std::strcmp(channel, "scripting") == 0) && (message != nullptr)) {
    g_lines.emplace_back(message);
  }
}

bool write_driver() {
  FILE *file = nullptr;
#ifdef _WIN32
  if ((fopen_s(&file, kDriverPath, "wb") != 0) || (file == nullptr)) {
    return false;
  }
#else
  file = std::fopen(kDriverPath, "wb");
  if (file == nullptr) {
    return false;
  }
#endif
  const std::size_t length = std::strlen(kDriver);
  const bool ok = std::fwrite(kDriver, 1U, length, file) == length;
  std::fclose(file);
  return ok;
}

/// Calls `function` and returns the one scripting Info line it logged, or
/// "<n lines>" when it logged another count.
std::string logged_by(const char *function) {
  g_lines.clear();
  if (!sc::call_script_function(function)) {
    return "<call failed>";
  }
  if (g_lines.size() != 1U) {
    return "<" + std::to_string(g_lines.size()) + " lines>";
  }
  return g_lines.front();
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!engine::core::initialize_logging() || !sc::initialize_scripting()) {
    std::fprintf(stderr, "FAIL: initialize\n");
    return 1;
  }
  auto world = std::unique_ptr<rt::World>(new (std::nothrow) rt::World());
  if (world == nullptr) {
    return 1;
  }
  engine::core::ServiceLocator serviceLocator{};
  rt::bind_scripting_runtime(world.get(), serviceLocator);

  engine::tests::TestContext ctx;
  const bool loaded = write_driver() && sc::load_script(kDriverPath);
  ctx.check(loaded, "driver loaded");
  if (loaded) {
    ctx.check(engine::core::log_register_sink(&capture, nullptr), "log sink");
    const std::string values = logged_by("print_values");
    ctx.check(values == "lua_print_driver.lua:1: a\t1\tnil",
              "print logs its arguments, tab-joined, at the caller's line");
    const std::string nothing = logged_by("print_nothing");
    ctx.check(nothing == "lua_print_driver.lua:2: ",
              "an empty print logs an empty line");
    const std::string object = logged_by("print_object");
    ctx.check(object == "lua_print_driver.lua:4: at\tpoint(1, 2)",
              "a value prints through its __tostring");
    if ((values.find("a\t1\tnil") == std::string::npos) ||
        (object.find("point(1, 2)") == std::string::npos)) {
      std::fprintf(stderr, "logged: [%s] [%s] [%s]\n", values.c_str(),
                   nothing.c_str(), object.c_str());
    }
    engine::core::log_unregister_sink(&capture, nullptr);
  }

  sc::shutdown_scripting();
  static_cast<void>(std::remove(kDriverPath));
  engine::core::shutdown_logging();
  return ctx.finish("lua_print_log");
}
