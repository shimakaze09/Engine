// Verifies Lua input-binding ingress validation (audit M-21 remainder,
// issue #80): out-of-range binding/axis-source type enums, indices and
// codes that do not fit the int range, wrap-prone rebind indices, and
// non-integer values are refused at the binding layer with a false
// return, while every in-range registration still succeeds through the
// production Lua entry point.

#include <cstdio>
#include <cstring>

#include "engine/core/input.h"
#include "engine/core/input_map.h"
#include "engine/core/logging.h"
#include "engine/scripting/scripting.h"

namespace {

constexpr const char *kScriptPath = "input_ingress_lua_test.lua";

/// Writes the Lua fixture to disk for the production load path.
bool write_script_file(const char *contents) noexcept {
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
  const std::size_t len = std::strlen(contents);
  const bool ok = (std::fwrite(contents, 1U, len, file) == len);
  return (std::fclose(file) == 0) && ok;
}

/// Script under test: each function errors on any accepted invalid value
/// so call_script_function reports the contract violation.
constexpr const char *kScriptContents =
    "function refuse_bad_action_type()\n"
    "    if engine.add_input_action('bad_type', {{ type = 999, code = 5 }})\n"
    "    then error('out-of-range binding type accepted') end\n"
    "    local probe = { type = 0, code = 5 }\n"
    "    if engine.rebind_action('bad_type', 0, probe) then\n"
    "        error('refused action was still registered')\n"
    "    end\n"
    "end\n"
    "function refuse_bad_axis_type()\n"
    "    if engine.add_input_axis('bad_axis', {{ type = 7 }}) then\n"
    "        error('out-of-range axis source type accepted')\n"
    "    end\n"
    "end\n"
    "function refuse_truncating_code()\n"
    "    local big = (2 ^ 32) + 5\n"
    "    if engine.add_input_action('trunc', {{ type = 0, code = big }})\n"
    "    then error('int-truncating code accepted') end\n"
    "    if engine.add_input_axis('trunc_axis',\n"
    "                             {{ type = 1, axis_index = big }})\n"
    "    then error('int-truncating axis_index accepted') end\n"
    "end\n"
    "function refuse_wrapping_rebind()\n"
    "    if not engine.add_input_action('wrap',\n"
    "                                   {{ type = 0, code = engine.KEY_A }})\n"
    "    then error('valid registration failed') end\n"
    "    local nb = { type = 0, code = engine.KEY_B }\n"
    "    if engine.rebind_action('wrap', 2 ^ 32, nb) then\n"
    "        error('uint32-wrapping binding_index accepted')\n"
    "    end\n"
    "    if engine.rebind_action('wrap', -1, nb) then\n"
    "        error('negative binding_index accepted')\n"
    "    end\n"
    "end\n"
    "function refuse_non_integer_fields()\n"
    "    if engine.add_input_action('frac', {{ type = 0.5, code = 5 }})\n"
    "    then error('fractional type accepted') end\n"
    "    if engine.add_input_action('nan_scale',\n"
    "                               {{ type = 3, code = 0,\n"
    "                                  axis_scale = 0 / 0 }})\n"
    "    then error('NaN axis_scale accepted') end\n"
    "    if engine.register_action('big_key', 2 ^ 40) then\n"
    "        error('int-overflowing register_action key accepted')\n"
    "    end\n"
    "    if engine.register_axis('big_axis', 0, 2 ^ 40) then\n"
    "        error('int-overflowing register_axis key accepted')\n"
    "    end\n"
    "end\n"
    "function query_boundaries()\n"
    "    if engine.is_key_down(2 ^ 32) then\n"
    "        error('int-truncating scancode reported a key down')\n"
    "    end\n"
    "    if engine.is_gamepad_button_down(2 ^ 32) then\n"
    "        error('int-truncating gamepad button reported down')\n"
    "    end\n"
    "    if engine.gamepad_axis_value(2 ^ 32) ~= 0 then\n"
    "        error('int-truncating gamepad axis returned a value')\n"
    "    end\n"
    "end\n"
    "function accept_valid_registrations()\n"
    "    local b = {\n"
    "        { type = 0, code = engine.KEY_SPACE },\n"
    "        { type = 3, code = 1, axis_threshold = 0.25,\n"
    "          axis_scale = -1.0 },\n"
    "    }\n"
    "    if not engine.add_input_action('good', b) then\n"
    "        error('valid add_input_action refused')\n"
    "    end\n"
    "    local s = {\n"
    "        { type = 0, negative_key = engine.KEY_A,\n"
    "          positive_key = engine.KEY_D },\n"
    "        { type = 1, axis_index = 0, dead_zone = 0.2 },\n"
    "    }\n"
    "    if not engine.add_input_axis('good_axis', s) then\n"
    "        error('valid add_input_axis refused')\n"
    "    end\n"
    "    local nb = { type = 1, code = 2 }\n"
    "    if not engine.rebind_action('good', 1, nb) then\n"
    "        error('valid rebind_action refused')\n"
    "    end\n"
    "    if not engine.rebind_action('good', 2, nb) then\n"
    "        error('valid append rebind_action refused')\n"
    "    end\n"
    "end\n";

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

  int result = 0;
  if (!write_script_file(kScriptContents)) {
    result = 3;
  }
  if ((result == 0) && !engine::scripting::load_script(kScriptPath)) {
    result = 4;
  }

  const char *cases[] = {
      "refuse_bad_action_type",   "refuse_bad_axis_type",
      "refuse_truncating_code",   "refuse_wrapping_rebind",
      "refuse_non_integer_fields", "query_boundaries",
      "accept_valid_registrations",
  };
  int caseCode = 10;
  for (const char *name : cases) {
    if ((result == 0) && !engine::scripting::call_script_function(name)) {
      result = caseCode;
    }
    ++caseCode;
  }

  static_cast<void>(std::remove(kScriptPath));
  engine::scripting::shutdown_scripting();
  engine::core::shutdown_input_mapper();
  engine::core::shutdown_input();

  if (result == 0) {
    std::printf("input_ingress_lua_test: all checks passed\n");
  }
  return result;
}
