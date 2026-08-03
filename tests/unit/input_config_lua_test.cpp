// Verifies the Lua input-config sandbox boundary (audit N-06): caller
// names for engine.save_input_config/load_input_config resolve strictly
// under the platform save directory, escape attempts (absolute paths,
// drive designators, backslashes, ".." segments) are refused, authored
// files outside the save directory survive byte-for-byte, and a
// legitimate name still round-trips through the production Lua entry
// point.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "engine/core/input.h"
#include "engine/core/input_map.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/scripting/scripting.h"

namespace {

constexpr const char *kScriptPath = "input_config_lua_test.lua";
constexpr const char *kAuthoredPath = "input_config_lua_scene.json";
constexpr const char *kAuthoredContent = "{\"authored\":true}";
constexpr const char *kSandboxedSceneName = "input_config_lua_scene.json";
constexpr const char *kConfigName = "input_config_lua_test_bindings.json";

/// Writes raw bytes to a file for the authored-fixture setup.
bool write_raw_file(const char *path, const char *content) noexcept {
  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t len = std::strlen(content);
  const bool ok = (std::fwrite(content, 1U, len, file) == len);
  return (std::fclose(file) == 0) && ok;
}

/// Reads the whole file; empty string when missing.
std::string read_raw_file(const char *path) {
  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "rb");
#endif
  if (file == nullptr) {
    return {};
  }
  char buffer[512] = {};
  const std::size_t read = std::fread(buffer, 1U, sizeof(buffer) - 1U, file);
  std::fclose(file);
  return std::string(buffer, read);
}

/// Builds "<savedir>/<name>"; empty string when unavailable.
std::string save_dir_path(const char *name) {
  char saveDir[512] = {};
  if (!engine::core::platform_get_save_dir(saveDir, sizeof(saveDir))) {
    return {};
  }
  return std::string(saveDir) + "/" + name;
}

/// Script under test: every function errors on an unexpected result so
/// call_script_function returns false on any contract violation.
constexpr const char *kScriptContents =
    "function setup_bindings()\n"
    "    local b = { { type = 0, code = engine.KEY_SPACE } }\n"
    "    if not engine.add_input_action('cfg_probe', b) then\n"
    "        error('add_input_action failed')\n"
    "    end\n"
    "end\n"
    "function refuse_escapes()\n"
    "    local bad = {\n"
    "        '../escape.json',\n"
    "        '/abs/escape.json',\n"
    "        'C:\\\\escape.json',\n"
    "        'a\\\\b.json',\n"
    "        '..',\n"
    "        'a/../escape.json',\n"
    "        '',\n"
    "    }\n"
    "    for _, p in ipairs(bad) do\n"
    "        if engine.save_input_config(p) then\n"
    "            error('save accepted: ' .. p)\n"
    "        end\n"
    "        if engine.load_input_config(p) then\n"
    "            error('load accepted: ' .. p)\n"
    "        end\n"
    "    end\n"
    "end\n"
    "function sandbox_scene_name()\n"
    "    if not engine.save_input_config('input_config_lua_scene.json') then\n"
    "        error('sandboxed save failed')\n"
    "    end\n"
    "end\n"
    "function save_named_config()\n"
    "    if not engine.save_input_config("
    "'input_config_lua_test_bindings.json') then\n"
    "        error('named save failed')\n"
    "    end\n"
    "end\n"
    "function probe_missing()\n"
    "    local nb = { type = 0, code = engine.KEY_SPACE }\n"
    "    if engine.rebind_action('cfg_probe', 0, nb) then\n"
    "        error('probe should be gone after reset')\n"
    "    end\n"
    "end\n"
    "function reload_named_config()\n"
    "    if not engine.load_input_config("
    "'input_config_lua_test_bindings.json') then\n"
    "        error('named load failed')\n"
    "    end\n"
    "    local nb = { type = 0, code = engine.KEY_SPACE }\n"
    "    if not engine.rebind_action('cfg_probe', 0, nb) then\n"
    "        error('probe missing after reload')\n"
    "    end\n"
    "end\n";

/// Removes every file the test may have created.
void cleanup() noexcept {
  static_cast<void>(std::remove(kScriptPath));
  static_cast<void>(std::remove(kAuthoredPath));
  const std::string sandboxedScene = save_dir_path(kSandboxedSceneName);
  if (!sandboxedScene.empty()) {
    static_cast<void>(std::remove(sandboxedScene.c_str()));
  }
  const std::string config = save_dir_path(kConfigName);
  if (!config.empty()) {
    static_cast<void>(std::remove(config.c_str()));
  }
}

} // namespace

/// Runs this executable or test program.
int main() {
  static_cast<void>(engine::core::initialize_logging());
  if (!engine::core::initialize_input() ||
      !engine::core::initialize_input_mapper()) {
    return 1;
  }
  if (!engine::scripting::initialize_scripting()) {
    return 2;
  }

  cleanup();
  int result = 0;
  char saveDir[512] = {};
  std::error_code ec{};

  if (!engine::core::platform_get_save_dir(saveDir, sizeof(saveDir))) {
    result = 3;
  }
  if (result == 0) {
    std::filesystem::create_directories(saveDir, ec);
    if (ec) {
      result = 4;
    }
  }
  if ((result == 0) && !write_raw_file(kAuthoredPath, kAuthoredContent)) {
    result = 5;
  }
  if ((result == 0) && !write_raw_file(kScriptPath, kScriptContents)) {
    result = 6;
  }
  if ((result == 0) && !engine::scripting::load_script(kScriptPath)) {
    result = 7;
  }

  if ((result == 0) &&
      !engine::scripting::call_script_function("setup_bindings")) {
    result = 10;
  }
  if ((result == 0) &&
      !engine::scripting::call_script_function("refuse_escapes")) {
    result = 11;
  }
  if ((result == 0) && (read_raw_file(kAuthoredPath) != kAuthoredContent)) {
    result = 12;
  }

  if ((result == 0) &&
      !engine::scripting::call_script_function("sandbox_scene_name")) {
    result = 13;
  }
  if ((result == 0) && (read_raw_file(kAuthoredPath) != kAuthoredContent)) {
    result = 14;
  }
  if ((result == 0) &&
      read_raw_file(save_dir_path(kSandboxedSceneName).c_str()).empty()) {
    result = 15;
  }

  if ((result == 0) &&
      !engine::scripting::call_script_function("save_named_config")) {
    result = 16;
  }
  if (result == 0) {
    engine::core::shutdown_input_mapper();
    if (!engine::core::initialize_input_mapper()) {
      result = 17;
    }
  }
  if ((result == 0) &&
      !engine::scripting::call_script_function("probe_missing")) {
    result = 18;
  }
  if ((result == 0) &&
      !engine::scripting::call_script_function("reload_named_config")) {
    result = 19;
  }

  cleanup();
  engine::scripting::shutdown_scripting();
  engine::core::shutdown_input_mapper();
  engine::core::shutdown_input();

  if (result == 0) {
    std::printf("input_config_lua_test: all checks passed\n");
  } else {
    std::printf("input_config_lua_test: FAILED with code %d\n", result);
  }
  return result;
}
