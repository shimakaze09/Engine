// Verifies engine::open_project on scratch projects: a project opens from
// its directory and from its .project document alike, filling exactly the
// config's content fields; each refusal (no path, nothing found, not a
// project file, two documents, a malformed document, a missing content
// root, startup scene or main script) names its reason and leaves the
// caller's storage and config untouched; and a project opened this way
// bootstraps headless with its per-user data named by its GUID. A
// project's script limits reach the config (the engine's defaults where it
// sets none, whatever the caller held) and the running VM at bootstrap,
// and a later run of a project that sets none is back on the defaults; its
// collision layers and save limit reach the config and the running engine
// the same way, and bootstrap refuses a save limit of 0. A
// project's packages open with it, each mounted at packages/<name>; one
// whose folder is missing refuses the open; and a package's assets are
// catalogued under their own identities and its scripts load.
// The bundled sample project opens and its document is in canonical form.

#include "engine/project.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#include "../asset_root.h"
#include "../test_harness.h"
#include "engine/content/asset_catalog.h"
#include "engine/content/asset_metadata.h"
#include "engine/content/asset_sidecar.h"
#include "engine/content/project_document.h"
#include "engine/core/logging.h"
#include "engine/core/project_data.h"
#include "engine/core/vfs.h"
#include "engine/runtime/collision_layers.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/save_data.h"
#include "engine/scripting/script_limits.h"
#include "engine/scripting/scripting.h"

namespace {

namespace fs = std::filesystem;
using engine::ProjectOpenFailureKind;

engine::tests::TestContext g_tests;

constexpr engine::core::AssetGuid kGuid{0x5a17f00d0badcafeULL,
                                        0x1234567890abcdefULL};

bool write_text(const fs::path &path, const char *text) {
  std::error_code ec{};
  fs::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary);
  out << text;
  return out.good();
}

engine::content::ProjectDocument make_document(const char *mainScript) {
  engine::content::ProjectDocument doc{};
  std::snprintf(doc.name, sizeof(doc.name), "%s", "Island");
  std::snprintf(doc.version, sizeof(doc.version), "%s", "0.1.0");
  doc.guid = kGuid;
  std::snprintf(doc.contentRoot, sizeof(doc.contentRoot), "%s", "assets");
  std::snprintf(doc.cacheRoot, sizeof(doc.cacheRoot), "%s", ".cache");
  std::snprintf(doc.scenes[0], sizeof(doc.scenes[0]), "%s",
                "assets/main.scene");
  doc.sceneCount = 1U;
  std::snprintf(doc.startupScene, sizeof(doc.startupScene), "%s",
                "assets/main.scene");
  std::snprintf(doc.mainScript, sizeof(doc.mainScript), "%s", mainScript);
  return doc;
}

/// A complete project in `dir`: Island.project, assets/main.scene and,
/// when `withScript`, assets/main.lua named by the document.
bool make_project(const fs::path &dir, bool withScript) {
  std::error_code ec{};
  fs::remove_all(dir, ec);
  fs::create_directories(dir / "assets", ec);
  const engine::content::ProjectDocument doc =
      make_document(withScript ? "assets/main.lua" : "");
  const std::string file = (dir / "Island.project").string();
  return !ec && engine::content::write_project_document(file.c_str(), doc) &&
         write_text(dir / "assets" / "main.scene", "{}") &&
         (!withScript || write_text(dir / "assets" / "main.lua", "-- main"));
}

/// The config and storage a refusal must leave exactly as they were.
struct Untouched final {
  engine::EngineConfig config{};
  engine::ProjectStorage storage{};

  Untouched() { std::snprintf(storage.contentRoot, 8U, "%s", "before"); }

  bool unchanged() const {
    const engine::EngineConfig defaults{};
    return (config.assetRoot == defaults.assetRoot) &&
           (config.editorAssetRoot == defaults.editorAssetRoot) &&
           (config.editorScenePath == defaults.editorScenePath) &&
           (config.mainScriptPath == defaults.mainScriptPath) &&
           !engine::core::asset_guid_is_valid(config.core.projectGuid) &&
           (config.scriptInstructionLimit == defaults.scriptInstructionLimit) &&
           (config.scriptMemoryLimitBytes == defaults.scriptMemoryLimitBytes) &&
           (config.packageCount == 0U) && (config.packages == nullptr) &&
           (std::strcmp(storage.contentRoot, "before") == 0) &&
           (storage.document.name[0] == '\0');
  }
};

/// Opens `path` and checks it is refused as `kind` with nothing changed,
/// and, when `detailHas` is given, that the refusal's detail names it.
void expect_refusal(const char *path, ProjectOpenFailureKind kind,
                    const char *what, const char *detailHas = nullptr) {
  Untouched target{};
  const auto opened = engine::open_project(path, &target.storage,
                                           &target.config);
  const bool refused = !opened.has_value() && (opened.error().kind == kind) &&
                       target.unchanged();
  const bool detailed =
      (detailHas == nullptr) ||
      (!opened.has_value() &&
       (std::strstr(opened.error().detail, detailHas) != nullptr));
  if (refused && !detailed) {
    std::fprintf(stderr, "  detail '%s' does not name '%s'\n",
                 opened.error().detail, detailHas);
  }
  g_tests.check(refused && detailed, what);
}

/// The document's text with its engine stamp replaced by `stamp`, or
/// removed when `stamp` is empty.
bool restamp(const fs::path &file, const std::string &stamp) {
  std::ifstream in(file, std::ios::binary);
  std::string text((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  in.close();
  const std::size_t at = text.find(",\n    \"engine\": \"");
  const std::size_t end =
      (at == std::string::npos) ? std::string::npos : text.find('"', at + 17U);
  if (end == std::string::npos) {
    return false;
  }
  text.replace(at, end + 1U - at,
               stamp.empty() ? std::string()
                             : (",\n    \"engine\": \"" + stamp + "\""));
  return write_text(file, text.c_str());
}

/// A project another engine wrote: a newer one is refused before anything
/// else is read, an older one and one from before the stamp open (#137).
void test_engine_stamp(const fs::path &root) {
  const fs::path dir = root / "stamped";
  const fs::path file = dir / "Island.project";
  const engine::content::ProjectEngineVersion running =
      engine::content::running_engine_version();
  const std::string newer = std::to_string(running.majorVersion + 1U) + ".0.0";

  g_tests.check(make_project(dir, true) && restamp(file, newer),
                "a project stamped by a newer engine");
  expect_refusal(dir.string().c_str(), ProjectOpenFailureKind::NewerEngine,
                 "a project a newer engine saved is refused, nothing changed, "
                 "naming the version that saved it",
                 newer.c_str());
  const std::string runningText = std::to_string(running.majorVersion) + "." +
                                  std::to_string(running.minorVersion) + "." +
                                  std::to_string(running.patchVersion);
  expect_refusal(dir.string().c_str(), ProjectOpenFailureKind::NewerEngine,
                 "and this engine's", ("this is " + runningText).c_str());

  engine::ProjectStorage storage{};
  engine::EngineConfig config{};
  g_tests.check(
      restamp(file, "0.0.0") &&
          engine::open_project(dir.string().c_str(), &storage, &config)
              .has_value() &&
          storage.document.engine.set &&
          (storage.document.engine.majorVersion == 0U) &&
          (storage.document.engine.patchVersion == 0U),
      "a project an older engine saved opens, its stamp read");

  engine::ProjectStorage unstamped{};
  engine::EngineConfig unstampedConfig{};
  g_tests.check(restamp(file, "") &&
                    engine::open_project(dir.string().c_str(), &unstamped,
                                         &unstampedConfig)
                        .has_value() &&
                    !unstamped.document.engine.set,
                "a project from before the stamp opens, unstamped");
}

void test_opens(const fs::path &root) {
  const fs::path dir = root / "island";
  g_tests.check(make_project(dir, true), "write a complete project");
  const std::string dirText = dir.string();
  const std::string expectedRoot = (dir / "assets").generic_string();

  engine::EngineConfig config{};
  config.engineRoot = "kept";
  engine::ProjectStorage storage{};
  const auto byDir = engine::open_project(dirText.c_str(), &storage, &config);
  g_tests.check(byDir.has_value(), "a project opens from its directory");
  g_tests.check(
      (std::strcmp(config.assetMount, "assets") == 0) &&
          (std::strcmp(config.assetRoot, expectedRoot.c_str()) == 0) &&
          (std::strcmp(config.editorAssetRoot, expectedRoot.c_str()) == 0) &&
          (std::strcmp(config.editorScenePath, "assets/main.scene") == 0) &&
          (std::strcmp(config.mainScriptPath, "assets/main.lua") == 0) &&
          (config.core.projectGuid == kGuid) &&
          (std::strcmp(config.engineRoot, "kept") == 0),
      "the content fields point at the project and the rest are kept");
  g_tests.check(
      (std::strcmp(storage.projectFile,
                   (dir / "Island.project").generic_string().c_str()) ==
       0) &&
          (std::strcmp(storage.projectDirectory,
                       dir.generic_string().c_str()) == 0) &&
          (std::strcmp(storage.document.name, "Island") == 0),
      "the storage holds the absolute document, directory and document");

  engine::EngineConfig byFileConfig{};
  engine::ProjectStorage byFileStorage{};
  const std::string fileText = (dir / "Island.project").string();
  g_tests.check(engine::open_project(fileText.c_str(), &byFileStorage,
                                     &byFileConfig)
                        .has_value() &&
                    (std::strcmp(byFileConfig.assetRoot,
                                 expectedRoot.c_str()) == 0),
                "the same project opens from its document");

  const fs::path noScript = root / "no_script";
  g_tests.check(make_project(noScript, false), "write a scriptless project");
  engine::EngineConfig scriptless{};
  engine::ProjectStorage scriptlessStorage{};
  const std::string noScriptText = noScript.string();
  g_tests.check(engine::open_project(noScriptText.c_str(), &scriptlessStorage,
                                     &scriptless)
                        .has_value() &&
                    (scriptless.mainScriptPath[0] == '\0'),
                "a project without a main script opens with none");
}

void test_refusals(const fs::path &root) {
  expect_refusal(nullptr, ProjectOpenFailureKind::InvalidPath,
                 "a null path is refused");
  expect_refusal("", ProjectOpenFailureKind::InvalidPath,
                 "an empty path is refused");
  const std::string missing = (root / "does_not_exist").string();
  expect_refusal(missing.c_str(), ProjectOpenFailureKind::NotFound,
                 "a path naming nothing is refused, saying what it is not",
                 "not a directory or a .project document");

  const fs::path empty = root / "empty";
  std::error_code ec{};
  fs::create_directories(empty, ec);
  const std::string emptyText = empty.string();
  expect_refusal(emptyText.c_str(), ProjectOpenFailureKind::NotFound,
                 "a directory with no document is refused, saying so",
                 "holds no .project document");

  const fs::path notProject = root / "island" / "assets" / "main.scene";
  const std::string notProjectText = notProject.string();
  expect_refusal(notProjectText.c_str(), ProjectOpenFailureKind::NotFound,
                 "a file that is not a .project document is refused");

  const fs::path two = root / "two";
  g_tests.check(make_project(two, true), "write a project to duplicate");
  fs::copy_file(two / "Island.project", two / "Copy.project", ec);
  const std::string twoText = two.string();
  expect_refusal(twoText.c_str(), ProjectOpenFailureKind::Ambiguous,
                 "a directory with two documents is refused, naming one",
                 "Copy.project");
  expect_refusal(twoText.c_str(), ProjectOpenFailureKind::Ambiguous,
                 "and naming the other", "Island.project");
  const std::string oneOfTwo = (two / "Copy.project").string();
  Untouched named{};
  g_tests.check(engine::open_project(oneOfTwo.c_str(), &named.storage,
                                     &named.config)
                    .has_value(),
                "naming one of two documents opens it");

  const fs::path malformed = root / "malformed";
  g_tests.check(write_text(malformed / "Broken.project", "{\"schema"),
                "write a malformed document");
  const std::string malformedText = malformed.string();
  Untouched broken{};
  const auto refused = engine::open_project(malformedText.c_str(),
                                            &broken.storage, &broken.config);
  g_tests.check(
      !refused.has_value() &&
          (refused.error().kind == ProjectOpenFailureKind::DocumentRefused) &&
          (refused.error().document.kind ==
           engine::content::ProjectReadFailureKind::Malformed) &&
          broken.unchanged() && (refused.error().document.reason[0] != '\0') &&
          (std::strcmp(refused.error().detail,
                       refused.error().document.reason) == 0),
      "a malformed document is refused with the codec's reason, carried as "
      "the refusal's detail");

  const fs::path noContent = root / "no_content";
  g_tests.check(make_project(noContent, true), "write a project to strip");
  fs::remove_all(noContent / "assets", ec);
  const std::string noContentText = noContent.string();
  expect_refusal(noContentText.c_str(),
                 ProjectOpenFailureKind::ContentRootMissing,
                 "a missing content root is refused, naming it", "assets");

  const fs::path noScene = root / "no_scene";
  g_tests.check(make_project(noScene, true), "write a project to strip");
  fs::remove(noScene / "assets" / "main.scene", ec);
  const std::string noSceneText = noScene.string();
  expect_refusal(noSceneText.c_str(),
                 ProjectOpenFailureKind::StartupSceneMissing,
                 "a missing startup scene is refused, naming it", "main.scene");

  const fs::path noScript = root / "script_gone";
  g_tests.check(make_project(noScript, true), "write a project to strip");
  fs::remove(noScript / "assets" / "main.lua", ec);
  const std::string noScriptText = noScript.string();
  expect_refusal(noScriptText.c_str(),
                 ProjectOpenFailureKind::MainScriptMissing,
                 "a main script the document names but is missing is "
                 "refused, naming it",
                 "main.lua");
}

/// A project opened by path bootstraps, and its per-user data is named by
/// its GUID rather than by where it lives.
void test_bootstrap(const fs::path &root) {
  const fs::path dir = root / "island";
  const std::string dirText = dir.string();
  engine::EngineConfig config{};
  engine::ProjectStorage storage{};
  if (!engine::open_project(dirText.c_str(), &storage, &config).has_value()) {
    g_tests.fail("open the project to bootstrap");
    return;
  }
  config.core.platform.headless = true;
  const std::string engineRoot = engine::tests::engine_root_path();
  config.engineRoot = engineRoot.c_str();
  config.bootstrapMeshPath = "project_open_missing.mesh";
  if (!engine::bootstrap(config)) {
    g_tests.fail("an opened project bootstraps headless");
    return;
  }
  char directory[1024] = {};
  const bool named =
      engine::core::project_data_dir(directory, sizeof(directory));
  const std::string text(directory);
  g_tests.check(named && (text.size() >= 32U) &&
                    (text.compare(text.size() - 32U, 32U,
                                  "5a17f00d0badcafe1234567890abcdef") == 0),
                "the running project's data is named by its GUID");
  engine::shutdown();
}

/// A project that sets limits, in `dir`: 2,500,000 instructions, an
/// unlimited allocator and 16 MiB save slots; it also names layer 3
/// "Player" and ignores the Player-layer 4 pair.
bool make_limited_project(const fs::path &dir) {
  if (!make_project(dir, false)) {
    return false;
  }
  engine::content::ProjectDocument doc = make_document("");
  doc.scriptLimits.instructionLimitSet = true;
  doc.scriptLimits.instructionLimit = 2500000U;
  doc.scriptLimits.memoryLimitSet = true;
  doc.scriptLimits.memoryLimitMiB = 0U;
  std::snprintf(doc.collisionLayers.names[3],
                sizeof(doc.collisionLayers.names[3]), "%s", "Player");
  engine::content::set_collision_layer_pair(&doc.collisionLayers, 3U, 4U,
                                            false);
  doc.saveSettings.maxSlotMiBSet = true;
  doc.saveSettings.maxSlotMiB = 16U;
  const std::string file = (dir / "Island.project").string();
  return engine::content::write_project_document(file.c_str(), doc);
}

bool bootstrap_headless(engine::EngineConfig config) {
  config.core.platform.headless = true;
  const std::string engineRoot = engine::tests::engine_root_path();
  config.engineRoot = engineRoot.c_str();
  config.bootstrapMeshPath = "project_open_missing.mesh";
  return engine::bootstrap(config);
}

void test_script_limits(const fs::path &root) {
  namespace sc = engine::scripting;
  const fs::path plainDir = root / "plain";
  const fs::path limitedDir = root / "limited";
  g_tests.check(make_project(plainDir, false) &&
                    make_limited_project(limitedDir),
                "write a project with limits and one without");
  const std::string plainText = plainDir.string();
  const std::string limitedText = limitedDir.string();

  engine::EngineConfig plain{};
  plain.scriptInstructionLimit = 5;
  plain.scriptMemoryLimitBytes = 5U;
  engine::ProjectStorage plainStorage{};
  g_tests.check(
      engine::open_project(plainText.c_str(), &plainStorage, &plain)
              .has_value() &&
          (plain.scriptInstructionLimit == sc::kDefaultInstructionLimit) &&
          (plain.scriptMemoryLimitBytes == sc::kDefaultMemoryLimit),
      "a project setting no limits runs at the engine's defaults, not at "
      "what the config held");

  engine::EngineConfig limited{};
  engine::ProjectStorage limitedStorage{};
  g_tests.check(
      engine::open_project(limitedText.c_str(), &limitedStorage, &limited)
              .has_value() &&
          (limited.scriptInstructionLimit == 2500000) &&
          (limited.scriptMemoryLimitBytes == 0U),
      "a project's own limits reach the config");
  g_tests.check(
      (std::strcmp(limited.collisionLayers.names[3], "Player") == 0) &&
          (limited.collisionLayers.collides[3] == ~(1U << 4U)) &&
          engine::content::collision_layers_are_default(plain.collisionLayers),
      "a project's collision layers reach the config, and a project naming "
      "none has the defaults");
  g_tests.check(
      (limited.saveSlotLimitBytes == 16U * 1024U * 1024U) &&
          (plain.saveSlotLimitBytes ==
           engine::runtime::kDefaultSaveSlotLimitBytes),
      "a project's save limit reaches the config, and a project setting "
      "none has the default");

  engine::EngineConfig cleared = limited;
  engine::configure_without_project(&cleared);
  g_tests.check(
      (cleared.scriptInstructionLimit == sc::kDefaultInstructionLimit) &&
          (cleared.scriptMemoryLimitBytes == sc::kDefaultMemoryLimit) &&
          engine::content::collision_layers_are_default(
              cleared.collisionLayers) &&
          (cleared.saveSlotLimitBytes ==
           engine::runtime::kDefaultSaveSlotLimitBytes),
      "no project restores the default limits, layers and save limit");

  const engine::content::ProjectScriptLimits memoryOnly{false, 0U, true, 32U};
  const engine::ScriptLimits resolved =
      engine::project_script_limits(memoryOnly);
  g_tests.check((resolved.instructionLimit == sc::kDefaultInstructionLimit) &&
                    (resolved.memoryLimitBytes == 32U * 1024U * 1024U),
                "one limit set leaves the other at its default");

  if (!bootstrap_headless(limited)) {
    g_tests.fail("the project with limits bootstraps headless");
    return;
  }
  g_tests.check((sc::get_instruction_limit() == 2500000) &&
                    (sc::get_memory_limit() == 0U),
                "bootstrap puts the project's limits on the running VM");
  g_tests.check(engine::content::find_collision_layer(
                    engine::runtime::project_collision_layers(), "player") == 3,
                "bootstrap makes the project's layers the running ones");
  g_tests.check(engine::runtime::save_slot_limit() == 16U * 1024U * 1024U,
                "bootstrap makes the project's save limit the running one");
  engine::shutdown();
  if (!bootstrap_headless(plain)) {
    g_tests.fail("the project without limits bootstraps headless");
    return;
  }
  g_tests.check((sc::get_instruction_limit() == sc::kDefaultInstructionLimit) &&
                    (sc::get_memory_limit() == sc::kDefaultMemoryLimit),
                "the next run, of a project setting none, is back on the "
                "defaults");
  g_tests.check(engine::content::collision_layers_are_default(
                    engine::runtime::project_collision_layers()),
                "and its layers are the defaults again");
  g_tests.check(engine::runtime::save_slot_limit() ==
                    engine::runtime::kDefaultSaveSlotLimitBytes,
                "and its save limit is the default again");
  engine::shutdown();
  engine::EngineConfig badLimit = plain;
  badLimit.saveSlotLimitBytes = 0U;
  g_tests.check(!bootstrap_headless(badLimit),
                "bootstrap refuses a save limit of 0");
}

/// A project in `dir` depending on the package "ui_kit", whose folder
/// holds `lib/util.lua` with a sidecar identity (returned through
/// `outGuid`) unless `withFolder` is false. Its startup scene is the
/// empty-project template's, so a pipeline can open it.
bool make_packaged_project(const fs::path &dir, bool withFolder,
                           engine::core::AssetGuid *outGuid) {
  if (!make_project(dir, false)) {
    return false;
  }
  std::error_code ec{};
  const fs::path templateScene = fs::path(engine::tests::engine_root_path()) /
                                 "templates~/empty_project/assets/main.scene";
  fs::copy_file(templateScene, dir / "assets" / "main.scene",
                fs::copy_options::overwrite_existing, ec);
  engine::content::ProjectDocument doc = make_document("");
  std::snprintf(doc.packages[0].name, sizeof(doc.packages[0].name), "%s",
                "ui_kit");
  std::snprintf(doc.packages[0].source, sizeof(doc.packages[0].source), "%s",
                "packages/ui_kit");
  doc.packageCount = 1U;
  const std::string file = (dir / "Island.project").string();
  if (ec || !engine::content::write_project_document(file.c_str(), doc)) {
    return false;
  }
  if (!withFolder) {
    return true;
  }
  const fs::path script = dir / "packages" / "ui_kit" / "lib" / "util.lua";
  engine::content::AssetSidecar sidecar{};
  sidecar.guid = engine::content::generate_asset_guid();
  *outGuid = sidecar.guid;
  const std::string scriptText = script.string();
  return write_text(script, "ui_kit_loaded = true\n") &&
         engine::content::write_asset_sidecar(scriptText.c_str(), sidecar);
}

void test_packages(const fs::path &root) {
  const fs::path dir = root / "packaged";
  engine::core::AssetGuid guid{};
  g_tests.check(make_packaged_project(dir, true, &guid),
                "write a project depending on a package");
  const std::string dirText = dir.string();
  const std::string expectedRoot =
      (dir / "packages" / "ui_kit").lexically_normal().generic_string();

  engine::EngineConfig config{};
  engine::ProjectStorage storage{};
  g_tests.check(
      engine::open_project(dirText.c_str(), &storage, &config).has_value() &&
          (config.packageCount == 1U) && (config.packages != nullptr) &&
          (std::strcmp(config.packages[0].mount, "packages/ui_kit") == 0) &&
          (std::string(config.packages[0].root) == expectedRoot),
      "a project's package reaches the config, mounted at packages/<name>");

  engine::EngineConfig cleared = config;
  engine::configure_without_project(&cleared);
  g_tests.check((cleared.packageCount == 0U) && (cleared.packages == nullptr),
                "no project carries no packages");

  const fs::path missingDir = root / "missing_package";
  engine::core::AssetGuid unused{};
  g_tests.check(make_packaged_project(missingDir, false, &unused),
                "write a project whose package folder is missing");
  const std::string missingText = missingDir.string();
  expect_refusal(missingText.c_str(), ProjectOpenFailureKind::PackageMissing,
                 "a missing package folder refuses the open, untouched");

  config.core.platform.headless = true;
  const std::string engineRoot = engine::tests::engine_root_path();
  config.engineRoot = engineRoot.c_str();
  if (!engine::bootstrap(config)) {
    g_tests.fail("a project with a package bootstraps headless");
    return;
  }
  g_tests.check(engine::core::vfs_file_exists("packages/ui_kit/lib/util.lua"),
                "the package's files are reachable at its mount");
  {
    engine::EnginePipeline pipeline;
    if (pipeline.initialize(0U)) {
      const engine::core::AssetRef ref = engine::runtime::editor_asset_ref(
          engine::content::make_asset_id_from_path(
              "packages/ui_kit/lib/util.lua"));
      g_tests.check(engine::core::asset_ref_is_valid(ref) && (ref.guid == guid),
                    "the package's asset is catalogued under its own "
                    "identity");
      g_tests.check(
          engine::scripting::load_script("packages/ui_kit/lib/util.lua"),
          "a package's script loads from its mount");
      pipeline.teardown();
    } else {
      g_tests.fail("the pipeline initializes on a project with a package");
    }
  }
  engine::shutdown();
}

/// The bundled sample project opens, and its document is exactly what the
/// codec writes for it, so a hand edit cannot drift from the format.
void test_bundled_sample() {
  const std::string sample = engine::tests::sample_project_path();
  engine::EngineConfig config{};
  engine::ProjectStorage storage{};
  if (sample.empty() ||
      !engine::open_project(sample.c_str(), &storage, &config).has_value()) {
    g_tests.fail("the bundled sample project opens");
    return;
  }
  static char formatted[engine::content::kMaxProjectDocumentBytes] = {};
  std::size_t length = 0U;
  std::ifstream in(storage.projectFile, std::ios::binary);
  const std::string onDisk((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
  g_tests.check(engine::content::format_project_document(
                    storage.document, formatted, sizeof(formatted), &length) &&
                    (onDisk == std::string(formatted, length)),
                "the sample's document is in the codec's canonical form");
  g_tests.check(
      (std::strcmp(storage.document.name, "island") == 0) &&
          (std::strcmp(config.editorScenePath, "assets/main.scene") == 0),
      "the sample opens on its main scene");
}

} // namespace

/// Every refusal kind names its own reason, so the hub never shows the
/// catch-all for a kind it can meet.
void test_failure_text() {
  constexpr ProjectOpenFailureKind kKinds[] = {
      ProjectOpenFailureKind::InvalidPath,
      ProjectOpenFailureKind::NotFound,
      ProjectOpenFailureKind::Ambiguous,
      ProjectOpenFailureKind::DocumentRefused,
      ProjectOpenFailureKind::ContentRootMissing,
      ProjectOpenFailureKind::StartupSceneMissing,
      ProjectOpenFailureKind::MainScriptMissing,
      ProjectOpenFailureKind::PackageMissing,
      ProjectOpenFailureKind::PathTooLong,
      ProjectOpenFailureKind::NewerEngine};
  const char *fallback =
      engine::project_open_failure_text(static_cast<ProjectOpenFailureKind>(
          static_cast<int>(ProjectOpenFailureKind::NewerEngine) + 1));
  bool distinct = true;
  for (std::size_t i = 0U; i < std::size(kKinds); ++i) {
    const char *text = engine::project_open_failure_text(kKinds[i]);
    distinct = distinct && (text != nullptr) && (text[0] != '\0') &&
               (std::strcmp(text, fallback) != 0);
    for (std::size_t j = 0U; j < i; ++j) {
      distinct = distinct &&
                 (std::strcmp(
                      text, engine::project_open_failure_text(kKinds[j])) != 0);
    }
  }
  g_tests.check(distinct, "every refusal kind has its own reason text");
}

int main() {
  const fs::path root = fs::temp_directory_path() / "engine_project_open_test";
  std::error_code ec{};
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
  g_tests.check(engine::core::initialize_logging(), "initialize logging");

  test_opens(root);
  test_refusals(root);
  test_failure_text();
  test_engine_stamp(root);
  test_bundled_sample();
  engine::core::shutdown_logging();
  test_bootstrap(root);
  test_script_limits(root);
  test_packages(root);

  fs::remove_all(root, ec);
  return g_tests.finish("project_open");
}
