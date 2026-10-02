// Regression for #568: engine.save_data / engine.load_data either round-trip
// player data exactly or refuse with a diagnostic. On base the writer stored
// every number as a float and the loader copied strings through a
// truncating copy, so an integer above 2^24 or a string past the load
// buffer came back as a different value with no error. Driven through the
// production bindings with the save slot replaced by memory, so the
// observation point is the JSON the bindings write and read.
//
// Size is bounded by the save slot's document ceiling alone: thousands of
// keys and a 1 MiB string round-trip, and a table whose document passes the
// ceiling is refused with the previous save intact.
//
// A save that does not load is told apart from no save and is never
// overwritten: load_data answers a status beside the table ("absent",
// "corrupt" with where the document breaks, "unsupported" for a newer
// version), holds the slot so save_data refuses, and discard_save lifts
// the hold. The writer stamps the format version.
//
// Each binding takes a slot name (default "default"), slots are kept
// apart, a slot argument that is not a name token is a Lua argument error
// rather than a save somewhere else, engine.list_saves reports the slots
// the runtime lists, and engine.get_save_limit the project's limit.

#include "../test_harness.h"
#include "engine/core/logging.h"
#include "engine/core/service_locator.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"
#include "engine/scripting/runtime_services.h"
#include "engine/scripting/scripting.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <string>

namespace sc = engine::scripting;
namespace rt = engine::runtime;

namespace {

constexpr const char *kScriptPath = "save_data_bindings_test.lua";

/// The in-memory save slots standing in for the on-disk ones, holding up
/// to the limit the runtime enforces. g_slot is the default slot.
constexpr std::size_t kLimit = 4U * 1024U * 1024U;
std::map<std::string, std::string> g_slots{};
std::string g_slot{};
bool g_slotWritten = false;
std::string g_lastSlot{};

/// The default slot's hold, as the runtime keeps it: set by hold_game_save,
/// it refuses saves until discard_game_save moves the document aside.
bool g_held = false;
int g_holdCalls = 0;
std::string g_discarded{};
std::string g_loaded{};

bool memory_save(const char *slot, const char *json,
                 std::size_t length) noexcept {
  g_lastSlot = (slot != nullptr) ? slot : "";
  if ((json == nullptr) || (length > kLimit)) {
    return false;
  }
  if (g_lastSlot != "default") {
    g_slots[g_lastSlot].assign(json, length);
    return true;
  }
  if (g_held) {
    return false;
  }
  g_slot.assign(json, length);
  g_slotWritten = true;
  return true;
}

sc::GameSaveRead memory_load(const char *slot, const char **outPayload,
                             std::size_t *outLength) noexcept {
  g_lastSlot = (slot != nullptr) ? slot : "";
  *outPayload = nullptr;
  *outLength = 0U;
  if (g_lastSlot != "default") {
    const auto found = g_slots.find(g_lastSlot);
    if (found == g_slots.end()) {
      return sc::GameSaveRead::Absent;
    }
    g_loaded = found->second;
  } else {
    if (!g_slotWritten) {
      return sc::GameSaveRead::Absent;
    }
    g_loaded = g_slot;
  }
  *outPayload = g_loaded.c_str();
  *outLength = g_loaded.size();
  return sc::GameSaveRead::Ok;
}

void memory_hold(const char * /*slot*/) noexcept {
  g_held = true;
  ++g_holdCalls;
}

bool memory_discard(const char *slot) noexcept {
  if ((slot != nullptr) && (std::strcmp(slot, "default") != 0)) {
    g_slots.erase(slot);
    return true;
  }
  g_discarded = g_slot;
  g_slot.clear();
  g_slotWritten = false;
  g_held = false;
  return true;
}

std::size_t memory_list(sc::GameSaveSlotInfo *out,
                        std::size_t capacity) noexcept {
  std::size_t count = 0U;
  for (const auto &[name, document] : g_slots) {
    if (count < capacity) {
      std::snprintf(out[count].slot, sizeof(out[count].slot), "%s",
                    name.c_str());
      out[count].status =
          (name == "broken") ? sc::GameSaveRead::Corrupt : sc::GameSaveRead::Ok;
      out[count].savedAt = 1700000000;
      out[count].payloadBytes = document.size();
    }
    ++count;
  }
  return count;
}

std::size_t memory_limit() noexcept { return kLimit; }

/// Error lines the bindings logged that say where a document breaks.
int g_breakReports = 0;

void note_error(engine::core::LogLevel level, const char * /*channel*/,
                const char *message, void * /*userData*/) noexcept {
  if ((level == engine::core::LogLevel::Error) && (message != nullptr) &&
      (std::strstr(message, "breaks near byte") != nullptr)) {
    ++g_breakReports;
  }
}

/// Plants a document the production writer would never produce.
void plant_slot(const char *json) {
  g_slot.assign(json);
  g_slotWritten = true;
}

bool write_script_file(const std::string &script) noexcept {
  const char *code = script.c_str();
  std::FILE *file = nullptr;
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
  const bool ok = std::fwrite(code, 1U, length, file) == length;
  return (std::fclose(file) == 0) && ok;
}

// Each case raises on a failed expectation, so call_script_function
// reports it as false. `same` pins value and Lua number subtype alike.
// CEILING, the save ceiling in bytes, is defined ahead of this text.
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
    "function case_many_keys()\n"
    "  local t = {}\n"
    "  for i = 1, 5000 do t['key' .. i] = string.rep('v', 200) .. i end\n"
    "  same(t)\n"
    "end\n"
    "function case_long_string()\n"
    "  local period = {}\n"
    "  for i = 1, 90 do period[i] = string.char(32 + i) end\n"
    "  local blob = string.rep(table.concat(period), 11650)\n"
    "  blob = blob .. string.rep('~', 1024 * 1024 - #blob)\n"
    "  same({blob = blob, after = 'tail'})\n"
    "end\n"
    "function refuse_past_ceiling()\n"
    "  expect(engine.save_data({s = string.rep('s', CEILING)}) == false,\n"
    "         'accepted a document past the ceiling')\n"
    "  local t = {}\n"
    "  for i = 1, CEILING // 1024 do t['k' .. i] = string.rep('v', 1024) end\n"
    "  expect(engine.save_data(t) == false,\n"
    "         'accepted a many-key document past the ceiling')\n"
    "end\n"
    "function refuse_embedded_nul()\n"
    "  expect(engine.save_data({s = 'a\\0b'}) == false,\n"
    "         'accepted a string holding a NUL byte')\n"
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
    "  expect(engine.save_data({s = string.rep('s', CEILING)}) == false,\n"
    "         'oversize save accepted')\n"
    "  local back = engine.load_data()\n"
    "  expect(type(back) == 'table' and back.keep == 42, 'previous save "
    "lost')\n"
    "end\n"
    "function load_refused() expect(engine.load_data() == nil,\n"
    "                               'a corrupt save loaded') end\n"
    "function load_status(want)\n"
    "  local back, status = engine.load_data()\n"
    "  expect(status == want, 'status ' .. tostring(status) .. ', want ' ..\n"
    "         want)\n"
    "  expect((back ~= nil) == (want == 'ok'), 'table only with ok')\n"
    "end\n"
    "function want_absent() load_status('absent') end\n"
    "function want_corrupt() load_status('corrupt') end\n"
    "function want_unsupported() load_status('unsupported') end\n"
    "function want_ok() load_status('ok') end\n"
    "function save_refused() expect(engine.save_data({x = 1}) == false,\n"
    "                               'a held save was overwritten') end\n"
    "function discard_then_save()\n"
    "  expect(engine.discard_save() == true, 'discard refused')\n"
    "  expect(engine.save_data({fresh = 1}) == true, 'save after discard')\n"
    "  load_status('ok')\n"
    "end\n"
    "function slots_are_apart()\n"
    "  expect(engine.save_data({a = 1}, 'one') == true, 'slot one save')\n"
    "  expect(engine.save_data({b = 2}, 'two') == true, 'slot two save')\n"
    "  local one = engine.load_data('one')\n"
    "  local two = engine.load_data('two')\n"
    "  expect(one.a == 1 and one.b == nil, 'slot one holds its own')\n"
    "  expect(two.b == 2 and two.a == nil, 'slot two holds its own')\n"
    "  local _, status = engine.load_data('three')\n"
    "  expect(status == 'absent', 'an unsaved slot is absent')\n"
    "  expect(engine.discard_save('two') == true, 'slot two discard')\n"
    "  _, status = engine.load_data('two')\n"
    "  expect(status == 'absent', 'a discarded slot is absent')\n"
    "end\n"
    "function nil_slot_is_default()\n"
    "  expect(engine.save_data({d = 4}, nil) == true, 'nil slot save')\n"
    "  expect(engine.load_data(nil).d == 4, 'nil slot load')\n"
    "end\n"
    "function bad_slots_raise()\n"
    "  local bad = {'a/b', '', 'has space', string.rep('x', 32), 7, true}\n"
    "  for _, slot in ipairs(bad) do\n"
    "    local ok, err = pcall(engine.save_data, {x = 1}, slot)\n"
    "    expect(not ok and string.find(err, 'save slot', 1, true),\n"
    "           'save_data accepted slot ' .. tostring(slot))\n"
    "    expect(not pcall(engine.load_data, slot),\n"
    "           'load_data accepted slot ' .. tostring(slot))\n"
    "    expect(not pcall(engine.discard_save, slot),\n"
    "           'discard_save accepted slot ' .. tostring(slot))\n"
    "  end\n"
    "end\n"
    "function lists_slots()\n"
    "  local saves = engine.list_saves()\n"
    "  expect(#saves == 2, 'two slots listed, got ' .. #saves)\n"
    "  expect(saves[1].slot == 'broken' and saves[1].status == 'corrupt',\n"
    "         'a damaged slot lists as corrupt')\n"
    "  expect(saves[2].slot == 'one' and saves[2].status == 'ok' and\n"
    "         saves[2].saved_at == 1700000000 and saves[2].bytes > 0 and\n"
    "         saves[2].legacy == false, 'a slot lists its fields')\n"
    "  expect(engine.get_save_limit() == CEILING, 'the save limit')\n"
    "end\n";

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
  services.hold_game_save = &memory_hold;
  services.discard_game_save = &memory_discard;
  services.list_game_saves = &memory_list;
  services.game_save_limit = &memory_limit;
  sc::bind_runtime_services(&services, serviceLocator);

  engine::tests::TestContext ctx;
  const std::string script =
      "CEILING = " + std::to_string(kLimit) + "\n" + kScript;
  ctx.check(write_script_file(script), "write test script");
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
  ctx.check(sc::call_script_function("case_many_keys"), "5000 keys round-trip");
  ctx.check(g_slot.size() > 1024U * 1024U,
            "the 5000-key document is over 1 MiB");
  ctx.check(sc::call_script_function("case_long_string"),
            "a 1 MiB string round-trips");
  ctx.check(sc::call_script_function("refuse_past_ceiling"),
            "a document past the save ceiling is refused");
  ctx.check(sc::call_script_function("refuse_embedded_nul"),
            "a string holding a NUL byte is refused");
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

  // A save that does not load is told apart from no save and held.
  ctx.check(engine::core::initialize_logging() &&
                engine::core::log_register_sink(&note_error, nullptr),
            "log sink");
  memory_discard("default");
  g_holdCalls = 0;
  ctx.check(sc::call_script_function("want_absent") && (g_holdCalls == 0),
            "no save reads as absent and holds nothing");
  g_holdCalls = 0;
  plant_slot("{\"version\":1,\"entries\":[{\"k\":\"coins\",\"v\":");
  const std::string corrupt = g_slot;
  ctx.check(sc::call_script_function("want_corrupt") && (g_holdCalls == 1) &&
                (g_breakReports == 1),
            "a document that does not parse is corrupt, held, and logged "
            "with where it breaks");
  ctx.check(sc::call_script_function("save_refused") && (g_slot == corrupt),
            "save_data does not overwrite the held save");
  ctx.check(sc::call_script_function("discard_then_save") &&
                (g_discarded == corrupt),
            "discard_save moves the save aside and the next save starts "
            "fresh");
  g_holdCalls = 0;
  plant_slot("{\"version\":2,\"entries\":[]}");
  ctx.check(sc::call_script_function("want_unsupported") && (g_holdCalls == 1),
            "a newer build's save is unsupported and held");
  memory_discard("default");
  g_holdCalls = 0;
  plant_slot("{\"version\":\"one\",\"entries\":[]}");
  ctx.check(sc::call_script_function("want_corrupt") && (g_holdCalls == 1),
            "a version that is not a positive integer is corrupt");
  memory_discard("default");
  g_holdCalls = 0;
  plant_slot("{\"entries\":[{\"k\":\"coins\",\"v\":3}]}");
  ctx.check(sc::call_script_function("want_ok") && (g_holdCalls == 0),
            "a save from before the version key still loads");
  ctx.check(sc::call_script_function("case_one") &&
                (g_slot.find("\"version\":1") != std::string::npos),
            "the writer stamps the format version");
  ctx.check(sc::call_script_function("slots_are_apart") &&
                (g_lastSlot == "two"),
            "named slots are saved, loaded and discarded apart");
  ctx.check(sc::call_script_function("nil_slot_is_default") &&
                (g_lastSlot == "default"),
            "a nil slot is the default slot");
  ctx.check(sc::call_script_function("bad_slots_raise"),
            "a slot that is not a name token is an argument error");
  g_slots["broken"] = "x";
  ctx.check(sc::call_script_function("lists_slots"),
            "list_saves and get_save_limit report the runtime's slots");
  engine::core::log_unregister_sink(&note_error, nullptr);
  engine::core::shutdown_logging();

  std::remove(kScriptPath);
  sc::shutdown_scripting();
  return ctx.finish("save_data_bindings");
}
