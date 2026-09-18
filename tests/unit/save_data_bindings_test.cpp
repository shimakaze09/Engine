// Regression for #568: engine.save_data / engine.load_data either round-trip
// player data exactly or refuse with a diagnostic. On base the writer stored
// every number as a float and the loader copied strings through a
// truncating copy, so an integer above 2^24 or a string past the load
// buffer came back as a different value with no error. Driven through the
// production bindings with the save slot replaced by memory, so the
// observation point is the JSON the bindings write and read.

#include "../test_harness.h"
#include "engine/core/service_locator.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace sc = engine::scripting;
namespace rt = engine::runtime;

namespace {

constexpr const char *kScriptPath = "save_data_bindings_test.lua";

/// The in-memory save slot standing in for the on-disk one.
char g_slot[16U * 1024U] = {};
std::size_t g_slotLength = 0U;
bool g_slotWritten = false;

bool memory_save(const char *json, std::size_t length) noexcept {
  if ((json == nullptr) || (length > sizeof(g_slot))) {
    return false;
  }
  std::memcpy(g_slot, json, length);
  g_slotLength = length;
  g_slotWritten = true;
  return true;
}

bool memory_load(char *out, std::size_t capacity,
                 std::size_t *outLength) noexcept {
  if (!g_slotWritten || (out == nullptr) || (outLength == nullptr) ||
      (g_slotLength > capacity)) {
    return false;
  }
  std::memcpy(out, g_slot, g_slotLength);
  *outLength = g_slotLength;
  return true;
}

/// Plants a document the production writer would never produce.
void plant_slot(const char *json) noexcept {
  g_slotLength = std::strlen(json);
  std::memcpy(g_slot, json, g_slotLength);
  g_slotWritten = true;
}

bool write_script_file(const char *code) noexcept {
  std::FILE *file = std::fopen(kScriptPath, "wb");
  if (file == nullptr) {
    return false;
  }
  const std::size_t length = std::strlen(code);
  const bool ok = std::fwrite(code, 1U, length, file) == length;
  return (std::fclose(file) == 0) && ok;
}

// Each case raises on a failed expectation, so call_script_function
// reports it as false. `same` pins value and Lua number subtype alike.
constexpr const char *kScript =
    "local function expect(cond, what) if not cond then error(what) end end\n"
    "local function same(t)\n"
    "  expect(engine.save_data(t) == true, 'save refused')\n"
    "  local back = engine.load_data()\n"
    "  expect(type(back) == 'table', 'load returned no table')\n"
    "  for k, v in pairs(t) do\n"
    "    expect(back[k] == v, 'value differs for ' .. k)\n"
    "    expect(math.type(back[k]) == math.type(v),\n"
    "           'number subtype differs for ' .. k)\n"
    "  end\n"
    "  for k in pairs(back) do expect(t[k] ~= nil, 'extra key ' .. k) end\n"
    "end\n"
    "function case_empty() same({}) end\n"
    "function case_one() same({coins = 3}) end\n"
    "function case_2p24() same({a = 16777216, b = 16777217}) end\n"
    "function case_2p53() same({a = 9007199254740992,\n"
    "                           b = 9007199254740993}) end\n"
    "function case_int64_extremes()\n"
    "  same({lo = math.mininteger, hi = math.maxinteger})\n"
    "end\n"
    "function case_float() same({x = 0.1, y = -2.5e-7, z = 1e300}) end\n"
    "function case_bool_string() same({flag = false, name = 'hero'}) end\n"
    "function case_string_at_cap() same({s = string.rep('s', 255)}) end\n"
    "function case_key_at_cap()\n"
    "  local t = {}\n"
    "  t[string.rep('k', 127)] = 1\n"
    "  same(t)\n"
    "end\n"
    "function case_keys_at_cap()\n"
    "  local t = {}\n"
    "  for i = 1, 64 do t['k' .. i] = i end\n"
    "  same(t)\n"
    "end\n"
    "function refuse_keys_past_cap()\n"
    "  local t = {}\n"
    "  for i = 1, 65 do t['k' .. i] = i end\n"
    "  expect(engine.save_data(t) == false, 'accepted 65 keys')\n"
    "end\n"
    "function refuse_string_past_cap()\n"
    "  expect(engine.save_data({s = string.rep('s', 256)}) == false,\n"
    "         'accepted a 256-byte string')\n"
    "end\n"
    "function refuse_key_past_cap()\n"
    "  local t = {}\n"
    "  t[string.rep('k', 128)] = 1\n"
    "  expect(engine.save_data(t) == false, 'accepted a 128-byte key')\n"
    "end\n"
    "function refuse_nonfinite()\n"
    "  expect(engine.save_data({x = 0/0}) == false, 'accepted nan')\n"
    "  expect(engine.save_data({x = math.huge}) == false, 'accepted inf')\n"
    "end\n"
    "function refuse_unsupported()\n"
    "  expect(engine.save_data({t = {}}) == false, 'accepted a table value')\n"
    "end\n"
    "function previous_survives_failed_save()\n"
    "  expect(engine.save_data({keep = 42}) == true, 'first save refused')\n"
    "  expect(engine.save_data({s = string.rep('s', 256)}) == false,\n"
    "         'oversize save accepted')\n"
    "  local back = engine.load_data()\n"
    "  expect(type(back) == 'table' and back.keep == 42, 'previous save lost')\n"
    "end\n"
    "function load_refused() expect(engine.load_data() == nil,\n"
    "                               'a corrupt save loaded') end\n";

} // namespace

/// Runs this executable or test program.
int main() {
  if (!sc::initialize_scripting()) {
    std::fprintf(stderr, "FAIL: initialize_scripting\n");
    return 1;
  }
  auto world = std::unique_ptr<rt::World>(new (std::nothrow) rt::World());
  if (world == nullptr) {
    sc::shutdown_scripting();
    return 1;
  }
  engine::core::ServiceLocator serviceLocator{};
  rt::bind_scripting_runtime(world.get(), serviceLocator);
  sc::RuntimeServices services =
      *serviceLocator.get_service<sc::RuntimeServices>();
  services.save_game_data = &memory_save;
  services.load_game_data = &memory_load;
  sc::bind_runtime_services(&services, serviceLocator);

  engine::tests::TestContext ctx;
  ctx.check(write_script_file(kScript), "write test script");
  ctx.check(sc::load_script(kScriptPath), "load test script");

  ctx.check(sc::call_script_function("case_empty"), "empty table");
  ctx.check(sc::call_script_function("case_one"), "one integer key");
  ctx.check(sc::call_script_function("case_2p24"),
            "2^24 and 2^24 + 1 round-trip exactly");
  ctx.check(sc::call_script_function("case_2p53"),
            "2^53 and 2^53 + 1 round-trip exactly");
  ctx.check(sc::call_script_function("case_int64_extremes"),
            "mininteger and maxinteger round-trip exactly");
  ctx.check(sc::call_script_function("case_float"),
            "floats round-trip at double precision");
  ctx.check(sc::call_script_function("case_bool_string"),
            "booleans and strings round-trip");
  ctx.check(sc::call_script_function("case_string_at_cap"),
            "a 255-byte string round-trips");
  ctx.check(sc::call_script_function("case_key_at_cap"),
            "a 127-byte key round-trips");
  ctx.check(sc::call_script_function("case_keys_at_cap"),
            "64 keys round-trip");
  ctx.check(sc::call_script_function("refuse_keys_past_cap"),
            "65 keys are refused");
  ctx.check(sc::call_script_function("refuse_string_past_cap"),
            "a 256-byte string is refused");
  ctx.check(sc::call_script_function("refuse_key_past_cap"),
            "a 128-byte key is refused");
  ctx.check(sc::call_script_function("refuse_nonfinite"),
            "nan and inf are refused");
  ctx.check(sc::call_script_function("refuse_unsupported"),
            "a table value is refused");
  ctx.check(sc::call_script_function("previous_survives_failed_save"),
            "a refused save leaves the previous save loadable");

  // A save the writer never produces: a key past the load buffer must
  // refuse the whole load rather than truncate the key.
  plant_slot("{\"entries\":[{\"k\":\"kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk"
             "kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk"
             "kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk"
             "kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk"
             "\",\"v\":1}]}");
  ctx.check(sc::call_script_function("load_refused"),
            "a key past the load buffer refuses the load");

  std::remove(kScriptPath);
  sc::shutdown_scripting();
  return ctx.finish("save_data_bindings");
}
