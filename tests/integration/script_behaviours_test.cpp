// Verifies several Lua behaviours on one entity (#134) through the real
// dispatch functions: each enabled behaviour begins, ticks and ends in list
// order and reads its own property values; a disabled one gets nothing; one
// enabled after its entity began begins before its first tick; one that
// faults stops alone while the others run on; a behaviour that began ends
// even if disabled later, and one that never began never ends; a script
// reading another entity's property gets the first behaviour that declares
// it; and a hot reload reaches only the behaviours running the changed
// script.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <system_error>

#include "../test_harness.h"
#include "engine/core/service_locator.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace {

namespace sc = engine::scripting;
namespace rt = engine::runtime;

/// Writes contents to a relative path.
bool write_file_at(const char *path, const char *contents) noexcept {
  FILE *f = nullptr;
#ifdef _WIN32
  if (fopen_s(&f, path, "wb") != 0 || f == nullptr) {
    return false;
  }
#else
  f = std::fopen(path, "wb");
  if (f == nullptr) {
    return false;
  }
#endif
  const std::size_t len = std::strlen(contents);
  const bool ok = (std::fwrite(contents, 1U, len, f) == len);
  std::fclose(f);
  return ok;
}

/// Moves a file's timestamp forward so the module poll sees a change.
bool advance_mtime(const char *path) noexcept {
  std::error_code error{};
  const std::filesystem::file_time_type current =
      std::filesystem::last_write_time(path, error);
  if (error) {
    return false;
  }
  std::filesystem::last_write_time(path, current + std::chrono::seconds(2),
                                   error);
  return !error;
}

constexpr const char *kAlpha = "behaviours_alpha.lua";
constexpr const char *kBeta = "behaviours_beta.lua";
constexpr const char *kGamma = "behaviours_gamma.lua";
constexpr const char *kFaulty = "behaviours_faulty.lua";
constexpr const char *kProbe = "behaviours_probe.lua";
constexpr const char *kVerify = "behaviours_verify.lua";

/// A behaviour named `tag` that records each hook it receives in the
/// global trace, its tick with the value of its own `speed` property.
std::string traced_behaviour(const char *tag, const char *extraProperties) {
  std::string text = "local M = {}\n"
                     "M.properties = { speed = 1.0";
  text += extraProperties;
  text += " }\n"
          "local function log(s) trace = (trace or '') .. s .. ';' end\n"
          "function M.on_begin_play(self) log('";
  text += tag;
  text += ".begin') end\n"
          "function M.on_tick(self, dt) log('";
  text += tag;
  text += ".tick:' .. tostring(engine.get_property(self, 'speed'))) end\n"
          "function M.on_end_play(self) log('";
  text += tag;
  text += ".end') end\n"
          "return M\n";
  return text;
}

constexpr const char *kFaultyScript =
    "local M = {}\n"
    "local function log(s) trace = (trace or '') .. s .. ';' end\n"
    "function M.on_begin_play(self) log('f.begin') end\n"
    "function M.on_tick(self, dt) log('f.tick') error('faulty tick') end\n"
    "return M\n";

constexpr const char *kProbeScript =
    "local M = {}\n"
    "local function log(s) trace = (trace or '') .. s .. ';' end\n"
    "function M.on_tick(self, dt)\n"
    "  local host = engine.find_entity_by_name('Host')\n"
    "  log('p:' .. tostring(engine.get_property(host, 'speed')) .. ':' ..\n"
    "      tostring(engine.get_property(host, 'only_beta')))\n"
    "end\n"
    "return M\n";

bool write_fixtures() noexcept {
  return write_file_at(kAlpha, traced_behaviour("a", "").c_str()) &&
         write_file_at(kBeta, traced_behaviour("b", ", only_beta = 5").c_str()) &&
         write_file_at(kGamma, traced_behaviour("c", "").c_str()) &&
         write_file_at(kFaulty, kFaultyScript) &&
         write_file_at(kProbe, kProbeScript);
}

void remove_fixtures() noexcept {
  for (const char *path : {kAlpha, kBeta, kGamma, kFaulty, kProbe, kVerify}) {
    static_cast<void>(std::remove(path));
  }
}

/// True when the Lua trace equals `expected`; prints it otherwise. The
/// trace is cleared either way.
bool trace_is(const char *expected) noexcept {
  std::string verify = "function verify_trace()\n"
                       "  local got = trace or ''\n"
                       "  trace = ''\n"
                       "  if got ~= '";
  verify += expected;
  verify += "' then print('trace: ' .. got) error('trace ' .. got) end\n"
            "end\n";
  return write_file_at(kVerify, verify.c_str()) && sc::load_script(kVerify) &&
         sc::call_script_function("verify_trace");
}

rt::ScriptPropertyValue float_value(float value) noexcept {
  rt::ScriptPropertyValue out{};
  out.type = rt::ScriptPropertyType::Float;
  out.floatValue = value;
  return out;
}

/// The list running `paths`, all enabled.
rt::ScriptComponent list_of(std::initializer_list<const char *> paths) {
  rt::ScriptComponent list{};
  for (const char *path : paths) {
    static_cast<void>(rt::script_behaviour_append(&list, path));
  }
  return list;
}

/// Creates an entity running `list`, named `name` when given.
rt::Entity make_host(rt::World *world, const rt::ScriptComponent &list,
                     const char *name = nullptr) noexcept {
  const rt::Entity entity = world->create_entity();
  if (name != nullptr) {
    rt::NameComponent nameComponent{};
    std::snprintf(nameComponent.name, sizeof(nameComponent.name), "%s", name);
    static_cast<void>(world->add_name_component(entity, nameComponent));
  }
  return world->add_script_component(entity, list) ? entity : rt::kInvalidEntity;
}

/// Sets behaviour `index`'s enabled flag on the live list.
bool set_enabled(rt::World *world, rt::Entity entity, std::size_t index,
                 bool enabled) noexcept {
  rt::ScriptComponent list{};
  if (!world->get_script_component(entity, &list)) {
    return false;
  }
  list.behaviours[index].enabled = enabled;
  return world->add_script_component(entity, list);
}

void tick() noexcept { sc::dispatch_entity_scripts_update(1.0F / 60.0F); }

/// Order, per-behaviour values, disabled, enabled later, and disabled after
/// it began.
void test_lifecycle_per_behaviour(engine::tests::TestContext &ctx,
                                  rt::World *world) {
  rt::ScriptComponent list = list_of({kAlpha, kBeta, kGamma});
  list.behaviours[2].enabled = false;
  const rt::Entity host = make_host(world, list, "Host");
  rt::ScriptPropertiesComponent overrides{};
  ctx.check((host != rt::kInvalidEntity) &&
                rt::script_properties_set(&overrides, 0U, "speed",
                                          float_value(3.0F)) &&
                rt::script_properties_set(&overrides, 1U, "speed",
                                          float_value(7.0F)) &&
                world->add_script_properties(host, overrides),
            "host with three behaviours, the third disabled");

  sc::dispatch_entity_scripts_begin_play(world);
  ctx.check(trace_is("a.begin;b.begin;"),
            "begin play reaches each enabled behaviour in list order");
  ctx.check(world->begin_play_pending_count() == 0U,
            "the entity has begun once its enabled behaviours have");

  tick();
  ctx.check(trace_is("a.tick:3.0;b.tick:7.0;"),
            "each behaviour ticks with its own value of a shared property "
            "name, and the disabled one is not called");

  ctx.check(set_enabled(world, host, 2U, true), "enable the third");
  tick();
  ctx.check(trace_is("a.tick:3.0;b.tick:7.0;c.begin;c.tick:1.0;"),
            "a behaviour enabled after its entity began begins before its "
            "first tick, with its script's default");
  tick();
  ctx.check(trace_is("a.tick:3.0;b.tick:7.0;c.tick:1.0;"),
            "it begins once");

  ctx.check(set_enabled(world, host, 1U, false), "disable the second");
  tick();
  ctx.check(trace_is("a.tick:3.0;c.tick:1.0;"),
            "a disabled behaviour stops ticking");

  sc::dispatch_entity_scripts_end();
  ctx.check(trace_is("a.end;b.end;c.end;"),
            "every behaviour that began ends, a since-disabled one included");
  static_cast<void>(world->destroy_entity(host));
}

/// A behaviour that never began never ends.
void test_unbegun_behaviour_never_ends(engine::tests::TestContext &ctx,
                                       rt::World *world) {
  rt::ScriptComponent list = list_of({kAlpha, kBeta});
  list.behaviours[1].enabled = false;
  const rt::Entity host = make_host(world, list);
  ctx.check(host != rt::kInvalidEntity, "host with a disabled behaviour");
  sc::dispatch_entity_scripts_begin_play(world);
  sc::dispatch_entity_scripts_end();
  ctx.check(trace_is("a.begin;a.end;"),
            "the behaviour that stayed disabled gets neither begin nor end");
  static_cast<void>(world->destroy_entity(host));
}

/// One behaviour faulting does not stop the entity's others.
void test_fault_is_per_behaviour(engine::tests::TestContext &ctx,
                                 rt::World *world) {
  const rt::Entity host = make_host(world, list_of({kFaulty, kAlpha}));
  ctx.check(host != rt::kInvalidEntity, "host with a faulty behaviour");
  sc::dispatch_entity_scripts_begin_play(world);
  tick();
  tick();
  ctx.check(trace_is("f.begin;a.begin;f.tick;a.tick:1.0;a.tick:1.0;"),
            "the faulted behaviour is skipped from then on while the next "
            "one keeps ticking");
  sc::dispatch_entity_scripts_end();
  ctx.check(trace_is("a.end;"), "a faulted behaviour is not ended");
  static_cast<void>(world->destroy_entity(host));
}

/// Another entity's script reads the first behaviour declaring a name.
void test_get_property_from_outside(engine::tests::TestContext &ctx,
                                    rt::World *world) {
  const rt::Entity host = make_host(world, list_of({kAlpha, kBeta}), "Host");
  rt::ScriptPropertiesComponent overrides{};
  ctx.check((host != rt::kInvalidEntity) &&
                rt::script_properties_set(&overrides, 0U, "speed",
                                          float_value(3.0F)) &&
                rt::script_properties_set(&overrides, 1U, "speed",
                                          float_value(7.0F)) &&
                world->add_script_properties(host, overrides),
            "host with overrides");
  const rt::Entity probe = make_host(world, list_of({kProbe}));
  sc::dispatch_entity_scripts_begin_play(world);
  static_cast<void>(trace_is("a.begin;b.begin;"));
  tick();
  ctx.check(trace_is("a.tick:3.0;b.tick:7.0;p:3.0:5;"),
            "from outside, a name resolves to the first behaviour declaring "
            "it: speed to the first's value, only_beta to the second's "
            "default");
  sc::dispatch_entity_scripts_end();
  static_cast<void>(trace_is("a.end;b.end;"));
  static_cast<void>(world->destroy_entity(host));
  static_cast<void>(world->destroy_entity(probe));
}

/// A reload reaches only the behaviours running the changed script.
void test_reload_reaches_its_behaviour(engine::tests::TestContext &ctx,
                                       rt::World *world) {
  const rt::Entity host = make_host(world, list_of({kAlpha, kBeta}));
  ctx.check(host != rt::kInvalidEntity, "host");
  sc::dispatch_entity_scripts_begin_play(world);
  static_cast<void>(trace_is("a.begin;b.begin;"));
  const std::string reloaded =
      "local M = {}\n"
      "M.properties = { speed = 1.0, only_beta = 5 }\n"
      "local function log(s) trace = (trace or '') .. s .. ';' end\n"
      "function M.on_reload(self, state) log('b.reload') end\n"
      "function M.on_tick(self, dt) log('b2.tick') end\n"
      "return M\n";
  ctx.check(write_file_at(kBeta, reloaded.c_str()) && advance_mtime(kBeta),
            "change the second script");
  tick();
  ctx.check(trace_is("a.tick:1.0;b.reload;b2.tick;"),
            "only the changed script's behaviour is told it reloaded, and "
            "the other runs on unchanged");
  sc::dispatch_entity_scripts_end();
  static_cast<void>(trace_is("a.end;"));
  static_cast<void>(world->destroy_entity(host));
}

} // namespace

int main() {
  if (!sc::initialize_scripting()) {
    std::fprintf(stderr, "FAIL: initialize_scripting\n");
    return 1;
  }
  auto world = std::unique_ptr<rt::World>(new (std::nothrow) rt::World());
  if ((world == nullptr) || !write_fixtures()) {
    remove_fixtures();
    sc::shutdown_scripting();
    return 1;
  }
  engine::core::ServiceLocator serviceLocator{};
  rt::bind_scripting_runtime(world.get(), serviceLocator);

  engine::tests::TestContext ctx;
  test_lifecycle_per_behaviour(ctx, world.get());
  sc::clear_entity_script_modules();
  test_unbegun_behaviour_never_ends(ctx, world.get());
  sc::clear_entity_script_modules();
  test_fault_is_per_behaviour(ctx, world.get());
  sc::clear_entity_script_modules();
  test_get_property_from_outside(ctx, world.get());
  sc::clear_entity_script_modules();
  test_reload_reaches_its_behaviour(ctx, world.get());

  sc::clear_entity_script_modules();
  sc::shutdown_scripting();
  remove_fixtures();
  return ctx.finish("script_behaviours");
}
