// Verifies the editor's autosave and restart recovery (#137) through the
// production session paths (editor_autosave.h):
//  - an autosave copies the unsaved scene into the project data
//    directory, alternating two slots behind a manifest, and never writes
//    the authored file;
//  - the interval starts when the scene becomes unsaved, and a saved scene
//    forgets the manifest;
//  - a clean end removes the marker and the manifest; a crash (the marker
//    left behind) or a fatal offers the copy at the next start, and the
//    startup scene waits for the choice;
//  - Recover adopts the copy as the authored scene, unsaved; Inspect opens
//    it untitled; Discard forgets it; an unresolved offer survives a clean
//    end;
//  - a damaged manifest, a missing copy, a copy that does not load and a
//    failed write each leave the previous state, with a diagnostic;
//  - a fatal's recovery copy is recorded for the next start;
//  - the interval preference round-trips through the layout file.

#include "editor_autosave.h"
#include "editor_preferences.h"
#include "editor_scene_document.h"
#include "editor_scene_document_fixture.h"
#include "editor_session.h"
#include "engine/core/cvar.h"
#include "engine/core/logging.h"
#include "engine/core/project_data.h"
#include "engine/editor/editor.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"
#include "imgui.h"

#include "../test_harness.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <new>
#include <string>
#include <system_error>

namespace {

using namespace engine::editor;
using engine::runtime::Entity;
using engine::runtime::World;

namespace fs = std::filesystem;

engine::tests::TestContext t;
engine::tests::RecentScenesGuard g_recentGuard;

/// The test's project, under the editor asset root ("assets", relative to
/// the working directory) so a recovered scene passes the save jail.
std::string g_project;
std::string g_dataDir;

constexpr std::uint64_t kMinuteNs = 60ULL * 1000000000ULL;

std::string read_text(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

void write_text(const std::string &path, const std::string &text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

bool exists(const std::string &path) {
  std::error_code ec;
  return fs::exists(path, ec) && !ec;
}

std::string autosave_path(const char *name) {
  return g_dataDir + "/Autosave/" + name;
}

/// Starts each case from no autosave state on disk or in memory.
void clear_state() {
  autosave_reset();
  std::error_code ec;
  fs::remove_all(g_dataDir + "/Autosave", ec);
  fs::remove_all(g_dataDir + "/Recovery", ec);
}

bool add_named(World &world, const char *name) {
  const Entity entity = world.create_scene_object();
  engine::runtime::NameComponent component{};
  std::snprintf(component.name, sizeof(component.name), "%s", name);
  return (entity != engine::runtime::kInvalidEntity) &&
         world.add_name_component(entity, component);
}

bool has_named(const World &world, const char *name) {
  return world.find_entity_by_name(name) != engine::runtime::kInvalidEntity;
}

/// Attaches `world` holding one entity, saved as the authored scene, then
/// makes an unsaved edit: the state every recovery begins from.
bool stage_unsaved_scene(World &world, const std::string &authored) {
  editor_set_world(&world);
  if (!add_named(world, "Saved") || !perform_scene_save_as(authored.c_str())) {
    return false;
  }
  if (!add_named(world, "Unsaved")) {
    return false;
  }
  editor_session().document.unrecordedEdit = true;
  return scene_document_is_dirty();
}

/// A copy written, then the process gone without its end: the marker and
/// the manifest stay, the in-memory state does not.
bool crash_with_copy(const std::string &authored) {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if ((world == nullptr) || !stage_unsaved_scene(*world, authored) ||
      !autosave_begin_session() || !autosave_write_now()) {
    editor_set_world(nullptr);
    return false;
  }
  editor_set_world(nullptr);
  autosave_reset();
  return true;
}

void check_copy_never_writes_authored_file() {
  clear_state();
  const std::string authored = g_project + "/level.scene";
  std::unique_ptr<World> world(new (std::nothrow) World());
  t.check(stage_unsaved_scene(*world, authored), "an unsaved scene");
  const std::string authoredBytes = read_text(authored);

  t.check(autosave_begin_session(), "the session begins");
  t.check(exists(autosave_path("session.lock")), "the marker is written");
  t.check(autosave_pending_recovery() == nullptr,
          "a first start offers nothing");
  t.check(autosave_write_now(), "the first copy is written");
  t.check(read_text(authored) == authoredBytes,
          "the authored file is not written");
  const std::string manifest = read_text(autosave_path("autosave.json"));
  t.check(manifest.find("\"Autosave/scene-a.scene\"") != std::string::npos,
          "the manifest names slot a");
  t.check(manifest.find(authored) != std::string::npos,
          "the manifest names the authored scene");

  std::unique_ptr<World> copy(new (std::nothrow) World());
  t.check(engine::runtime::load_scene(*copy,
                                      autosave_path("scene-a.scene").c_str()) &&
              has_named(*copy, "Saved") && has_named(*copy, "Unsaved"),
          "the copy holds the unsaved scene");

  t.check(autosave_write_now(), "the second copy is written");
  t.check(read_text(autosave_path("autosave.json"))
                  .find("\"Autosave/scene-b.scene\"") != std::string::npos,
          "the second copy goes to slot b");
  t.check(exists(autosave_path("scene-a.scene")),
          "the previous copy stays until its slot is reused");
  t.check(read_text(authored) == authoredBytes,
          "the authored file is still not written");

  autosave_end_session();
  editor_set_world(nullptr);
}

void check_interval_and_saved_scene() {
  clear_state();
  const std::string authored = g_project + "/interval.scene";
  std::unique_ptr<World> world(new (std::nothrow) World());
  editor_set_world(world.get());
  t.check(add_named(*world, "A") && perform_scene_save_as(authored.c_str()),
          "a saved scene");
  set_autosave_minutes(1);
  t.check(autosave_begin_session(), "the session begins");

  const std::uint64_t t0 = 1000U * kMinuteNs;
  t.check(!autosave_tick(t0), "a saved scene is not copied");
  editor_session().document.unrecordedEdit = true;
  t.check(!autosave_tick(t0), "the interval starts at the first edit");
  t.check(!autosave_tick(t0 + kMinuteNs - 1U), "nothing before the interval");
  t.check(autosave_tick(t0 + kMinuteNs), "a copy when the interval ends");
  t.check(!autosave_tick(t0 + kMinuteNs + 1U),
          "the next interval starts at the copy");
  t.check(exists(autosave_path("autosave.json")), "the manifest names it");

  t.check(perform_scene_save(), "the scene is saved");
  t.check(!autosave_tick(t0 + 3U * kMinuteNs), "a saved scene is not copied");
  t.check(!exists(autosave_path("autosave.json")),
          "a saved scene forgets the manifest, so a crash never offers a "
          "copy older than the file");

  set_autosave_minutes(0);
  editor_session().document.unrecordedEdit = true;
  t.check(!autosave_tick(t0 + 10U * kMinuteNs) &&
              !autosave_tick(t0 + 100U * kMinuteNs),
          "0 minutes is off");
  set_autosave_minutes(-5);
  t.check(autosave_minutes() == 0, "a negative interval is off");
  set_autosave_minutes(500);
  t.check(autosave_minutes() == kMaxAutosaveMinutes, "the longest is 60");
  set_autosave_minutes(kDefaultAutosaveMinutes);

  autosave_end_session();
  editor_set_world(nullptr);
}

void check_clean_end_offers_nothing() {
  clear_state();
  const std::string authored = g_project + "/clean.scene";
  std::unique_ptr<World> world(new (std::nothrow) World());
  t.check(stage_unsaved_scene(*world, authored) && autosave_begin_session() &&
              autosave_write_now(),
          "a copy of an unsaved scene");
  autosave_end_session();
  t.check(!exists(autosave_path("session.lock")) &&
              !exists(autosave_path("autosave.json")),
          "a clean end removes the marker and the manifest");
  t.check(autosave_begin_session() && !autosave_recovery_pending(),
          "the next start offers nothing");
  autosave_end_session();
  editor_set_world(nullptr);
}

void check_recover_adopts_copy_unsaved() {
  clear_state();
  const std::string authored = g_project + "/recover.scene";
  t.check(crash_with_copy(authored), "a crash after a copy");
  const std::string authoredBytes = read_text(authored);

  std::unique_ptr<World> world(new (std::nothrow) World());
  editor_set_world(world.get());
  editor_session().document.startupScenePending = true;
  t.check(autosave_begin_session() && autosave_recovery_pending(),
          "the next start offers the copy");
  const AutosaveRecord *record = autosave_pending_recovery();
  t.check((record != nullptr) &&
              (std::strcmp(record->scenePath, authored.c_str()) == 0) &&
              (std::strcmp(record->sceneName, "recover.scene") == 0) &&
              !record->beforePlay && (record->savedAt[0] != '\0'),
          "the offer names the scene and when");
  scene_document_open_startup_scene();
  t.check(editor_session().document.startupScenePending,
          "the startup scene waits for the choice");

  t.check(autosave_choose_recovery(RecoveryChoice::Recover),
          "Recover loads the copy");
  t.check(has_named(*world, "Saved") && has_named(*world, "Unsaved"),
          "the world holds the unsaved scene");
  t.check(scene_document_has_path() &&
              (std::strcmp(scene_document_path(), authored.c_str()) == 0) &&
              scene_document_is_dirty(),
          "as the authored scene, unsaved");
  t.check(read_text(authored) == authoredBytes,
          "the authored file is not written by Recover");
  t.check(!autosave_recovery_pending(), "the offer is resolved");
  t.check(exists(autosave_path("autosave.json")),
          "the manifest stays until the recovered scene is saved");

  t.check(perform_scene_save(), "Save writes the recovered scene");
  std::unique_ptr<World> saved(new (std::nothrow) World());
  t.check(engine::runtime::load_scene(*saved, authored.c_str()) &&
              has_named(*saved, "Unsaved"),
          "the authored file now holds it");
  autosave_end_session();
  editor_set_world(nullptr);
}

void check_inspect_opens_untitled() {
  clear_state();
  const std::string authored = g_project + "/inspect.scene";
  t.check(crash_with_copy(authored), "a crash after a copy");
  const std::string authoredBytes = read_text(authored);

  std::unique_ptr<World> world(new (std::nothrow) World());
  editor_set_world(world.get());
  t.check(autosave_begin_session() &&
              autosave_choose_recovery(RecoveryChoice::Inspect),
          "Inspect loads the copy");
  t.check(has_named(*world, "Unsaved") && !scene_document_has_path() &&
              scene_document_is_dirty(),
          "as an untitled, unsaved scene");
  t.check(read_text(authored) == authoredBytes,
          "the authored file is unchanged");
  autosave_end_session();
  editor_set_world(nullptr);
}

void check_discard_forgets_copy() {
  clear_state();
  const std::string authored = g_project + "/discard.scene";
  t.check(crash_with_copy(authored), "a crash after a copy");

  std::unique_ptr<World> world(new (std::nothrow) World());
  editor_set_world(world.get());
  t.check(add_named(*world, "Untouched"), "the world before the choice");
  t.check(autosave_begin_session() &&
              autosave_choose_recovery(RecoveryChoice::Discard),
          "Discard");
  t.check(!autosave_recovery_pending() &&
              !exists(autosave_path("autosave.json")),
          "the offer and the manifest are gone");
  t.check(exists(autosave_path("scene-a.scene")),
          "the copy's file stays until a later autosave replaces it");
  t.check(has_named(*world, "Untouched") && !has_named(*world, "Unsaved"),
          "the world is not changed");
  autosave_end_session();
  t.check(autosave_begin_session() && !autosave_recovery_pending(),
          "a discarded copy is not offered again");
  autosave_end_session();
  editor_set_world(nullptr);
}

void check_unresolved_offer_survives_clean_end() {
  clear_state();
  const std::string authored = g_project + "/unresolved.scene";
  t.check(crash_with_copy(authored), "a crash after a copy");
  t.check(autosave_begin_session() && autosave_recovery_pending(),
          "the copy is offered");
  autosave_end_session();
  t.check(autosave_begin_session() && autosave_recovery_pending(),
          "quitting without a choice offers it again");
  autosave_end_session();
}

void check_fatal_end_keeps_offer() {
  clear_state();
  const std::string authored = g_project + "/fatal.scene";
  std::unique_ptr<World> world(new (std::nothrow) World());
  t.check(stage_unsaved_scene(*world, authored) && autosave_begin_session() &&
              autosave_write_now(),
          "a copy of an unsaved scene");
  autosave_note_fatal_exit();
  autosave_end_session();
  t.check(exists(autosave_path("session.lock")) &&
              exists(autosave_path("autosave.json")),
          "a fatal end keeps the marker and the manifest");
  t.check(autosave_begin_session() && autosave_recovery_pending(),
          "the next start offers the copy");
  autosave_end_session();
  editor_set_world(nullptr);
}

void check_damaged_manifests_offer_nothing() {
  const std::string authored = g_project + "/damaged.scene";
  struct Damage {
    const char *what;
    const char *find;
    const char *replace;
  };
  const Damage damages[] = {
      {"another version", "\"autosave\": 1", "\"autosave\": 2"},
      {"a parent step", "Autosave/scene-a.scene",
       "Autosave/../../escape.scene"},
      {"a folder other than Autosave and Recovery", "Autosave/scene-a.scene",
       "Elsewhere/scene-a.scene"},
      {"a missing field", "\"beforePlay\"", "\"beforeplay\""},
  };
  for (const Damage &damage : damages) {
    clear_state();
    t.check(crash_with_copy(authored), damage.what);
    std::string manifest = read_text(autosave_path("autosave.json"));
    const std::size_t at = manifest.find(damage.find);
    t.check(at != std::string::npos, damage.what);
    if (at != std::string::npos) {
      manifest.replace(at, std::strlen(damage.find), damage.replace);
    }
    write_text(autosave_path("autosave.json"), manifest);
    t.check(autosave_begin_session() && !autosave_recovery_pending(),
            damage.what);
    autosave_end_session();
  }

  clear_state();
  t.check(crash_with_copy(authored), "a crash after a copy");
  std::error_code ec;
  fs::remove(autosave_path("scene-a.scene"), ec);
  t.check(autosave_begin_session() && !autosave_recovery_pending(),
          "a copy that is gone is not offered");
  autosave_end_session();
}

void check_copy_that_does_not_load() {
  clear_state();
  const std::string authored = g_project + "/broken.scene";
  t.check(crash_with_copy(authored), "a crash after a copy");
  write_text(autosave_path("scene-a.scene"), "{ not a scene");

  std::unique_ptr<World> world(new (std::nothrow) World());
  editor_set_world(world.get());
  t.check(add_named(*world, "Before"), "the world before the choice");
  t.check(autosave_begin_session() && autosave_recovery_pending(),
          "the copy is offered");
  t.check(!autosave_choose_recovery(RecoveryChoice::Recover),
          "a copy that does not load is refused");
  t.check(autosave_recovery_pending() && (autosave_recovery_error()[0] != '\0'),
          "the offer stays, saying why");
  t.check(has_named(*world, "Before") && !scene_document_has_path(),
          "the world and the document are unchanged");
  t.check(autosave_choose_recovery(RecoveryChoice::Discard),
          "Discard still continues");
  autosave_end_session();
  editor_set_world(nullptr);
}

void check_failed_write_keeps_previous_copy() {
  clear_state();
  const std::string authored = g_project + "/fullslot.scene";
  std::unique_ptr<World> world(new (std::nothrow) World());
  t.check(stage_unsaved_scene(*world, authored) && autosave_begin_session() &&
              autosave_write_now(),
          "a first copy in slot a");
  const std::string manifest = read_text(autosave_path("autosave.json"));
  // A directory where slot b's file belongs: its write cannot commit.
  std::error_code ec;
  fs::create_directories(autosave_path("scene-b.scene") + "/blocked", ec);
  t.check(!autosave_write_now(), "the second copy fails");
  t.check(read_text(autosave_path("autosave.json")) == manifest,
          "the manifest still names slot a");
  fs::remove_all(autosave_path("scene-b.scene"), ec);
  t.check(
      autosave_write_now() &&
          (read_text(autosave_path("autosave.json")).find("scene-b.scene") !=
           std::string::npos),
      "the next copy goes to slot b once it can");
  autosave_end_session();
  editor_set_world(nullptr);
}

void check_fatal_copy_is_recorded() {
  clear_state();
  const std::string authored = g_project + "/recorded.scene";
  std::unique_ptr<World> world(new (std::nothrow) World());
  t.check(stage_unsaved_scene(*world, authored), "an unsaved scene");
  std::error_code ec;
  fs::create_directories(g_dataDir + "/Recovery", ec);
  const std::string copy = g_dataDir + "/Recovery/recorded-1.scene";
  t.check(engine::runtime::save_scene(*world, copy.c_str()), "a recovery copy");
  t.check(autosave_record_copy(copy.c_str(), true),
          "it is recorded without a begun session");
  t.check(!autosave_record_copy((g_project + "/outside.scene").c_str(), false),
          "a file outside the project data directory is refused");
  editor_set_world(nullptr);

  std::unique_ptr<World> next(new (std::nothrow) World());
  editor_set_world(next.get());
  const AutosaveRecord *record =
      autosave_begin_session() ? autosave_pending_recovery() : nullptr;
  t.check((record != nullptr) &&
              (std::strcmp(record->file, "Recovery/recorded-1.scene") == 0) &&
              record->beforePlay,
          "the next start offers it");
  t.check(autosave_choose_recovery(RecoveryChoice::Recover) &&
              has_named(*next, "Unsaved"),
          "and recovers it");
  autosave_end_session();
  editor_set_world(nullptr);
}

void check_interval_preference_round_trips() {
  ImGui::CreateContext();
  t.check(engine::core::initialize_cvars(), "initialize cvars");
  register_editor_preferences();
  const char stored[] = "[EnginePreferences][Editor]\n"
                        "AutosaveMinutes=12\n\n";
  ImGui::LoadIniSettingsFromMemory(stored, sizeof(stored) - 1U);
  t.check(autosave_minutes() == 12, "a stored interval is read");
  char section[512] = {};
  t.check((editor_preferences_section(section, sizeof(section)) > 0U) &&
              (std::strstr(section, "AutosaveMinutes=12\n") != nullptr),
          "and written back");
  const char malformed[] = "[EnginePreferences][Editor]\n"
                           "AutosaveMinutes=61\n\n";
  ImGui::LoadIniSettingsFromMemory(malformed, sizeof(malformed) - 1U);
  t.check(autosave_minutes() == 12, "an interval over 60 is ignored");
  set_autosave_minutes(kDefaultAutosaveMinutes);
  engine::core::shutdown_cvars();
  ImGui::DestroyContext();
}

} // namespace

int main() {
  static_cast<void>(engine::core::initialize_logging());
  std::error_code ec;
  const fs::path project =
      fs::weakly_canonical(fs::path("assets/engine_autosave_test"), ec);
  fs::create_directories(project, ec);
  g_project = project.generic_string();
  char dataDir[1024] = {};
  if (!engine::core::set_project_data_root(g_project.c_str()) ||
      !engine::core::project_data_dir(dataDir, sizeof(dataDir)) ||
      !g_recentGuard.arm((g_project + "/recent").c_str())) {
    std::fprintf(stderr, "the test project could not be set up\n");
    return 98;
  }
  g_dataDir = dataDir;

  check_copy_never_writes_authored_file();
  check_interval_and_saved_scene();
  check_clean_end_offers_nothing();
  check_recover_adopts_copy_unsaved();
  check_inspect_opens_untitled();
  check_discard_forgets_copy();
  check_unresolved_offer_survives_clean_end();
  check_fatal_end_keeps_offer();
  check_damaged_manifests_offer_nothing();
  check_copy_that_does_not_load();
  check_failed_write_keeps_previous_copy();
  check_fatal_copy_is_recorded();
  check_interval_preference_round_trips();

  clear_state();
  t.check(g_recentGuard.disarm(), "the real recent-scenes file is untouched");
  fs::remove_all(project, ec);
  engine::core::clear_project_data_root();
  engine::core::shutdown_logging();
  return t.finish("editor_autosave");
}
