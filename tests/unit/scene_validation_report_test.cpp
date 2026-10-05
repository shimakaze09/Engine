// Pins the scene loader's validation report: a dangling Transform parent,
// a script path and an animation controller path that name no file each
// load with a fallback and leave a named Warning in the report and a
// diagnostic naming the entity; a path under an unmounted prefix is not
// judged; a clean scene reports nothing; past capacity the report counts
// what it dropped. A key no reader looks up, at the root, on an entity,
// among the components or inside one, loads and is named as unknown_key
// in the report and the log, so it is not lost silently on the next
// save; keys retired on purpose and a scene this build saved report
// nothing.

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>

#include "../test_harness.h"
#include "engine/core/diagnostic.h"
#include "engine/core/logging.h"
#include "engine/core/validation_report.h"
#include "engine/core/vfs.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

namespace {

namespace rt = engine::runtime;
namespace core = engine::core;

std::uint32_t g_lastSceneWarningEntity = 0U;
char g_lastSceneWarningField[32] = {};
int g_sceneWarnings = 0;

void note_scene_record(const core::Diagnostic &record, void *) noexcept {
  if ((record.level == core::LogLevel::Warning) &&
      (std::strcmp(record.channel, "scene") == 0)) {
    g_lastSceneWarningEntity = record.entityPersistentId;
    std::snprintf(g_lastSceneWarningField, sizeof(g_lastSceneWarningField),
                  "%s", record.field);
    ++g_sceneWarnings;
  }
}

bool load(rt::World &world, const std::string &json,
          core::ValidationReport *report) noexcept {
  return rt::load_scene(world, json.c_str(), json.size(), nullptr, report);
}

const core::ValidationEntry *find_entry(const core::ValidationReport &report,
                                        const char *code) noexcept {
  for (std::size_t i = 0U; i < report.count; ++i) {
    if (std::strcmp(report.entries[i].code, code) == 0) {
      return &report.entries[i];
    }
  }
  return nullptr;
}

} // namespace

/// Runs this executable or test program.
int main() {
  engine::tests::TestContext ctx;
  ctx.check(core::initialize_logging(), "initialize logging");
  ctx.check(core::log_register_diagnostic_sink(&note_scene_record, nullptr),
            "register record sink");
  ctx.check(core::initialize_vfs() && core::mount("assets", "."),
            "mount the working directory as assets");

  std::unique_ptr<rt::World> world(new (std::nothrow) rt::World());
  if (world == nullptr) {
    return 1;
  }

  // --- A dangling parent loads as a root and is named ---
  {
    core::ValidationReport report{};
    g_sceneWarnings = 0;
    const std::string json =
        "{\"version\":6,\"entities\":[{\"persistentId\":7,\"components\":"
        "{\"Transform\":{\"position\":[1,2,3],\"parentId\":999}}}]}";
    ctx.check(load(*world, json, &report), "the scene with a dangling parent loads");
    ctx.check(world->alive_entity_count() == 1U, "the child is present");
    const core::ValidationEntry *entry = find_entry(report, "dangling_parent");
    ctx.check(entry != nullptr, "the report names the dangling parent");
    ctx.check((entry != nullptr) &&
                  (entry->severity == core::ValidationSeverity::Warning) &&
                  (entry->entityPersistentId == 7U) &&
                  (std::strcmp(entry->key, "999") == 0),
              "the entry carries the child and the missing id");
    ctx.check((g_sceneWarnings == 1) && (g_lastSceneWarningEntity == 7U) &&
                  (std::strcmp(g_lastSceneWarningField, "parentId") == 0),
              "the diagnostic names the child entity and the field");
  }

  // --- A missing script and controller under the mounted prefix ---
  {
    core::ValidationReport report{};
    const std::string json =
        "{\"version\":6,\"entities\":[{\"persistentId\":8,\"components\":"
        "{\"ScriptComponent\":\"assets/no_such_script_xyz.lua\","
        "\"AnimationComponent\":\"assets/no_such_controller_xyz.json\"}}]}";
    ctx.check(load(*world, json, &report), "the scene with missing files loads");
    const core::ValidationEntry *script = find_entry(report, "missing_script");
    const core::ValidationEntry *controller =
        find_entry(report, "missing_controller");
    ctx.check((script != nullptr) && (script->entityPersistentId == 8U) &&
                  (std::strcmp(script->key, "assets/no_such_script_xyz.lua") ==
                   0),
              "the missing script is named");
    ctx.check((controller != nullptr) &&
                  (controller->entityPersistentId == 8U),
              "the missing controller is named");
    ctx.check(report.count == 2U, "nothing else is reported");
  }

  // --- An unmounted prefix is not judged; a present file is clean ---
  {
    core::ValidationReport report{};
    const std::string json =
        "{\"version\":6,\"entities\":[{\"persistentId\":9,\"components\":"
        "{\"ScriptComponent\":\"elsewhere/script.lua\"}}]}";
    ctx.check(load(*world, json, &report) && report.clean(),
              "a path under an unmounted prefix is left alone");
    ctx.check(core::vfs_write_text("assets/scene_validation_present.lua",
                                   "-- present\n", 11U),
              "write a present script");
    core::ValidationReport presentReport{};
    const std::string presentJson =
        "{\"version\":6,\"entities\":[{\"persistentId\":10,\"components\":"
        "{\"ScriptComponent\":\"assets/scene_validation_present.lua\"}}]}";
    ctx.check(load(*world, presentJson, &presentReport) &&
                  presentReport.clean(),
              "a present script reports nothing");
    static_cast<void>(std::remove("scene_validation_present.lua"));
  }

  // --- Keys no reader looks up are named, at every level ---
  {
    core::ValidationReport report{};
    g_sceneWarnings = 0;
    const std::string json =
        "{\"version\":6,\"unknownRoot\":1,\"entities\":[{\"persistentId\":11,"
        "\"extraEntityKey\":true,\"components\":{\"Colider\":{\"shape\":"
        "\"box\"},\"Transform\":{\"position\":[1,2,3],\"futureField\":5}}}]}";
    ctx.check(load(*world, json, &report),
              "a scene with unknown keys still loads");
    const rt::Entity entity = world->find_entity_by_persistent_id(11U);
    rt::Transform transform{};
    ctx.check(world->get_transform(entity, &transform) &&
                  (transform.position.y == 2.0F),
              "the keys it knows are read");
    const char *const expected[] = {
        "unknownRoot", "entities[0].extraEntityKey",
        "entities[0].components.Colider",
        "entities[0].components.Transform.futureField"};
    bool named = report.count == 4U;
    for (std::size_t i = 0U; named && (i < 4U); ++i) {
      named =
          (std::strcmp(report.entries[i].code, "unknown_key") == 0) &&
          (report.entries[i].severity == core::ValidationSeverity::Warning) &&
          (std::strcmp(report.entries[i].key, expected[i]) == 0);
    }
    ctx.check(named, "the report names each unknown key by its path");
    ctx.check(g_sceneWarnings == 4, "each unknown key is logged once");
  }

  // --- Retired keys, and a scene this build saved, report nothing ---
  {
    core::ValidationReport report{};
    const std::string retired =
        "{\"version\":6,\"entities\":[{\"components\":{"
        "\"ReflectionProbeComponent\":{\"radius\":3.5,\"needsBake\":false,"
        "\"brdfLutResolution\":256}}}]}";
    ctx.check(load(*world, retired, &report) && report.clean(),
              "the retired probe fields load without a finding");

    const std::string authored =
        "{\"version\":6,\"gravity\":[0,-9.81,0],\"entities\":["
        "{\"persistentId\":20,\"components\":{\"name\":\"Crate\","
        "\"Transform\":{\"position\":[0,1,0]},\"RigidBody\":{},"
        "\"Collider\":{\"shape\":0,\"halfExtents\":[0.5,0.5,0.5]}}},"
        "{\"persistentId\":21,\"components\":{\"Transform\":{\"parentId\":20},"
        "\"LightComponent\":{\"type\":1},"
        "\"ReflectionProbeComponent\":{}}}]}";
    ctx.check(load(*world, authored, &report) && report.clean(),
              "the authored scene loads clean");
    std::unique_ptr<char[]> saved{};
    std::size_t savedSize = 0U;
    core::ValidationReport reloaded{};
    ctx.check(
        rt::save_scene(*world, &saved, &savedSize) &&
            load(*world, std::string(saved.get(), savedSize), &reloaded) &&
            reloaded.clean(),
        "every key the writer emits is read back");
  }

  // --- Script property overrides are read keys, not unknown ones ---
  // Their names are the script's, so the codec walks the object by
  // position; observed on 2026-10-05, engine_validate reported every
  // override in a saved scene as a key that would be lost on the next save.
  {
    ctx.check(core::vfs_write_text("assets/scene_validation_props.lua",
                                   "-- props\n", 9U),
              "write the overridden script");
    core::ValidationReport report{};
    g_sceneWarnings = 0;
    const std::string json =
        "{\"version\":6,\"entities\":[{\"persistentId\":30,\"components\":{"
        "\"ScriptComponent\":\"assets/scene_validation_props.lua\","
        "\"ScriptProperties\":{\"speed\":2.5,\"lives\":3,\"label\":\"x\","
        "\"armed\":true}}}]}";
    ctx.check(load(*world, json, &report) && report.clean() &&
                  (g_sceneWarnings == 0),
              "overrides load with no finding and no warning");
    std::unique_ptr<char[]> saved{};
    std::size_t savedSize = 0U;
    core::ValidationReport reloaded{};
    ctx.check(
        rt::save_scene(*world, &saved, &savedSize) &&
            load(*world, std::string(saved.get(), savedSize), &reloaded) &&
            reloaded.clean(),
        "saved overrides are read back with no finding");
    static_cast<void>(std::remove("scene_validation_props.lua"));
  }

  // --- Past 32 unknown keys the log stops listing them ---
  {
    core::ValidationReport report{};
    g_sceneWarnings = 0;
    std::string json = "{\"version\":6,\"entities\":[";
    for (unsigned i = 0U; i < 40U; ++i) {
      json += (i == 0U) ? "{\"bogus\":1}" : ",{\"bogus\":1}";
    }
    json += "]}";
    ctx.check(load(*world, json, &report) && (report.count == 40U),
              "every unknown key is in the report");
    ctx.check(g_sceneWarnings == 33,
              "32 are logged, then one line gives the total");
  }

  // --- A clean scene, and a report that fills ---
  {
    core::ValidationReport report{};
    report.count = 5U;
    ctx.check(load(*world, "{\"version\":6,\"entities\":[]}", &report) &&
                  report.clean(),
              "a clean scene resets and leaves the report empty");

    std::string json = "{\"version\":6,\"entities\":[";
    for (unsigned i = 0U; i < core::ValidationReport::kMaxEntries + 6U; ++i) {
      char entity[128] = {};
      std::snprintf(entity, sizeof(entity),
                    "%s{\"persistentId\":%u,\"components\":{\"Transform\":"
                    "{\"parentId\":900000}}}",
                    (i == 0U) ? "" : ",", 100U + i);
      json += entity;
    }
    json += "]}";
    ctx.check(load(*world, json, &report), "the many-fault scene loads");
    ctx.check((report.count == core::ValidationReport::kMaxEntries) &&
                  (report.dropped == 6U),
              "the report keeps the first findings and counts the rest");
    ctx.check(report.entries[0].entityPersistentId == 100U,
              "the first finding is the first entity's");
  }

  core::shutdown_vfs();
  core::log_unregister_diagnostic_sink(&note_scene_record, nullptr);
  core::shutdown_logging();
  return ctx.finish("scene_validation_report");
}
