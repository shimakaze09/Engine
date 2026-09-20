// Pins the scene loader's validation report: a dangling Transform parent,
// a script path and an animation controller path that name no file each
// load with a fallback and leave a named Warning in the report and a
// diagnostic naming the entity; a path under an unmounted prefix is not
// judged; a clean scene reports nothing; past capacity the report counts
// what it dropped.

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
        "{\"version\":3,\"entities\":[{\"persistentId\":7,\"components\":"
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
        "{\"version\":3,\"entities\":[{\"persistentId\":8,\"components\":"
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
        "{\"version\":3,\"entities\":[{\"persistentId\":9,\"components\":"
        "{\"ScriptComponent\":\"elsewhere/script.lua\"}}]}";
    ctx.check(load(*world, json, &report) && report.clean(),
              "a path under an unmounted prefix is left alone");
    ctx.check(core::vfs_write_text("assets/scene_validation_present.lua",
                                   "-- present\n", 11U),
              "write a present script");
    core::ValidationReport presentReport{};
    const std::string presentJson =
        "{\"version\":3,\"entities\":[{\"persistentId\":10,\"components\":"
        "{\"ScriptComponent\":\"assets/scene_validation_present.lua\"}}]}";
    ctx.check(load(*world, presentJson, &presentReport) &&
                  presentReport.clean(),
              "a present script reports nothing");
    static_cast<void>(std::remove("scene_validation_present.lua"));
  }

  // --- A clean scene, and a report that fills ---
  {
    core::ValidationReport report{};
    report.count = 5U;
    ctx.check(load(*world, "{\"version\":3,\"entities\":[]}", &report) &&
                  report.clean(),
              "a clean scene resets and leaves the report empty");

    std::string json = "{\"version\":3,\"entities\":[";
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
