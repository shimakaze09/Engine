// Verifies that the editor's static scan of script properties and the Lua
// VM agree on every script the engine ships (#134): each script under the
// sample project and the new-project template is scanned and also run in a
// plain Lua 5.4 VM, with `engine` stubbed so top-level code finds the
// functions it names. For every script the scan reads no diagnostic, finds
// exactly the properties the module's `properties` table holds, and gives
// each the type and default the VM evaluates. The samples declare
// properties at all, so the suite cannot pass by finding none.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "engine/scripting/script_property_scan.h"

#include "../asset_root.h"
#include "../test_harness.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace {

namespace fs = std::filesystem;
using engine::math::ScriptPropertyType;
using engine::scripting::ScriptPropertyDecl;
using engine::scripting::ScriptPropertySchema;

engine::tests::TestContext g_tests{};

/// `engine` for a script run outside the engine: every field is a function
/// that returns nothing, so top-level calls succeed without effects.
constexpr const char *kEngineStub =
    "engine = setmetatable({}, { __index = function() "
    "return function() end end })";

std::string read_file(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

/// True when the VM value at the top of the stack is the declared default
/// of the given type.
bool vm_matches(lua_State *state, const ScriptPropertyDecl &decl) {
  const engine::math::ScriptPropertyValue &value = decl.defaultValue;
  switch (value.type) {
  case ScriptPropertyType::Bool:
    return lua_isboolean(state, -1) &&
           ((lua_toboolean(state, -1) != 0) == value.boolValue);
  case ScriptPropertyType::Integer:
    return (lua_isinteger(state, -1) != 0) &&
           (lua_tointeger(state, -1) == value.integerValue);
  case ScriptPropertyType::Float:
    // A float property may take an integer default in the table form.
    return (lua_type(state, -1) == LUA_TNUMBER) &&
           (static_cast<float>(lua_tonumber(state, -1)) == value.floatValue);
  case ScriptPropertyType::String:
    return (lua_type(state, -1) == LUA_TSTRING) &&
           (std::strcmp(lua_tostring(state, -1), value.text) == 0);
  }
  return false;
}

/// Compares one script's scan with what the VM evaluates; returns how many
/// properties it declares.
std::size_t check_script(const fs::path &path) {
  const std::string name = path.filename().string();
  const std::string text = read_file(path);
  ScriptPropertySchema schema{};
  engine::scripting::scan_script_properties(text.data(), text.size(), &schema);
  if (schema.diagnosticCount != 0U) {
    std::printf("  %s line %u: %s\n", name.c_str(),
                static_cast<unsigned>(schema.diagnostics[0].line),
                schema.diagnostics[0].message);
  }
  g_tests.check(schema.diagnosticCount == 0U,
                "a shipped script's declarations all scan");

  lua_State *state = luaL_newstate();
  if (state == nullptr) {
    g_tests.fail("a Lua state is created");
    return 0U;
  }
  luaL_openlibs(state);
  const bool stubbed = luaL_dostring(state, kEngineStub) == LUA_OK;
  const bool ran = stubbed &&
                   (luaL_loadbuffer(state, text.data(), text.size(),
                                    name.c_str()) == LUA_OK) &&
                   (lua_pcall(state, 0, 1, 0) == LUA_OK);
  if (!ran) {
    std::printf("  %s: %s\n", name.c_str(), lua_tostring(state, -1));
    g_tests.fail("a shipped script runs in a plain VM");
    lua_close(state);
    return 0U;
  }
  std::size_t vmCount = 0U;
  if (lua_istable(state, -1) &&
      (lua_getfield(state, -1, "properties") == LUA_TTABLE)) {
    const int table = lua_gettop(state);
    lua_pushnil(state);
    while (lua_next(state, table) != 0) {
      ++vmCount;
      const char *key = lua_tostring(state, -2);
      const ScriptPropertyDecl *decl =
          (key != nullptr)
              ? engine::scripting::find_script_property(schema, key)
              : nullptr;
      bool matches = false;
      if (decl != nullptr) {
        if (lua_istable(state, -1)) {
          lua_getfield(state, -1, "default");
          matches = lua_isnil(state, -1) || vm_matches(state, *decl);
          lua_pop(state, 1);
        } else {
          matches = vm_matches(state, *decl);
        }
      }
      if (!matches) {
        std::printf("  %s: property '%s' differs\n", name.c_str(),
                    (key != nullptr) ? key : "?");
      }
      g_tests.check(matches,
                    "the scan finds each VM property with its type and "
                    "default");
      lua_pop(state, 1);
    }
  }
  g_tests.check(vmCount == schema.count,
                "the scan finds as many properties as the VM holds");
  lua_close(state);
  return schema.count;
}

} // namespace

/// Runs the scan/VM parity suite.
int main() {
  const std::string project = engine::tests::sample_project_path();
  const std::string engineRoot = engine::tests::engine_root_path();
  if (project.empty() || engineRoot.empty()) {
    g_tests.fail("the sample project and the engine content are found");
    return g_tests.finish("script property parity");
  }
  std::vector<fs::path> scripts;
  for (const fs::path &root :
       {fs::path(project), fs::path(engineRoot) / "templates~"}) {
    std::error_code ec{};
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end;
         it.increment(ec)) {
      if (it->is_regular_file() && (it->path().extension() == ".lua")) {
        scripts.push_back(it->path());
      }
    }
  }
  std::size_t declared = 0U;
  for (const fs::path &script : scripts) {
    declared += check_script(script);
  }
  std::printf("[parity] %zu scripts, %zu declared properties\n", scripts.size(),
              declared);
  g_tests.check(scripts.size() >= 5U, "the shipped scripts are found");
  // The samples' 19: the platform's 2, the rock's 3, the island player's
  // 5, the controller's 6 and the playground player's 3.
  g_tests.check(declared >= 19U,
                "the samples declare properties, so the suite is not vacuous");
  return g_tests.finish("script property parity");
}
