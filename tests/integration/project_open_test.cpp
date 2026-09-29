// Verifies engine::open_project on scratch projects: a project opens from
// its directory and from its .project document alike, filling exactly the
// config's content fields; each refusal (no path, nothing found, not a
// project file, two documents, a malformed document, a missing content
// root, startup scene or main script) names its reason and leaves the
// caller's storage and config untouched; and a project opened this way
// bootstraps headless with its per-user data named by its GUID. A
// project's script limits reach the config (the engine's defaults where it
// sets none, whatever the caller held) and the running VM at bootstrap,
// and a later run of a project that sets none is back on the defaults. A
// project's packages open with it, each mounted at packages/<name>; one
// whose folder is missing refuses the open; and a package's assets are
// catalogued under their own identities and its scripts load.
// The bundled sample project opens and its document is in canonical form.

#include "engine/project.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
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
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
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

void expect_refusal(const char *path, ProjectOpenFailureKind kind,
                    const char *what) {
  Untouched target{};
  const auto opened = engine::open_project(path, &target.storage,
                                           &target.config);
  g_tests.check(!opened.has_value() && (opened.error().kind == kind) &&
                    target.unchanged(),
                what);
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
                 "a path naming nothing is refused");

  const fs::path empty = root / "empty";
  std::error_code ec{};
  fs::create_directories(empty, ec);
  const std::string emptyText = empty.string();
  expect_refusal(emptyText.c_str(), ProjectOpenFailureKind::NotFound,
                 "a directory with no document is refused");

  const fs::path notProject = root / "island" / "assets" / "main.scene";
  const std::string notProjectText = notProject.string();
  expect_refusal(notProjectText.c_str(), ProjectOpenFailureKind::NotFound,
                 "a file that is not a .project document is refused");

  const fs::path two = root / "two";
  g_tests.check(make_project(two, true), "write a project to duplicate");
  fs::copy_file(two / "Island.project", two / "Copy.project", ec);
  const std::string twoText = two.string();
  expect_refusal(twoText.c_str(), ProjectOpenFailureKind::Ambiguous,
                 "a directory with two documents is refused");
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
          broken.unchanged(),
      "a malformed document is refused with the codec's reason");

  const fs::path noContent = root / "no_content";
  g_tests.check(make_project(noContent, true), "write a project to strip");
  fs::remove_all(noContent / "assets", ec);
  const std::string noContentText = noContent.string();
  expect_refusal(noContentText.c_str(),
                 ProjectOpenFailureKind::ContentRootMissing,
                 "a missing content root is refused");

  const fs::path noScene = root / "no_scene";
  g_tests.check(make_project(noScene, true), "write a project to strip");
  fs::remove(noScene / "assets" / "main.scene", ec);
  const std::string noSceneText = noScene.string();
  expect_refusal(noSceneText.c_str(),
                 ProjectOpenFailureKind::StartupSceneMissing,
                 "a missing startup scene is refused");

  const fs::path noScript = root / "script_gone";
  g_tests.check(make_project(noScript, true), "write a project to strip");
  fs::remove(noScript / "assets" / "main.lua", ec);
  const std::string noScriptText = noScript.string();
  expect_refusal(noScriptText.c_str(),
                 ProjectOpenFailureKind::MainScriptMissing,
                 "a main script the document names but is missing is "
                 "refused");
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

/// A project that sets limits, in `dir`: 2,500,000 instructions and an
/// unlimited allocator.
bool make_limited_project(const fs::path &dir) {
  if (!make_project(dir, false)) {
    return false;
  }
  engine::content::ProjectDocument doc = make_document("");
  doc.scriptLimits.instructionLimitSet = true;
  doc.scriptLimits.instructionLimit = 2500000U;
  doc.scriptLimits.memoryLimitSet = true;
  doc.scriptLimits.memoryLimitMiB = 0U;
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

  engine::EngineConfig cleared = limited;
  engine::configure_without_project(&cleared);
  g_tests.check(
      (cleared.scriptInstructionLimit == sc::kDefaultInstructionLimit) &&
          (cleared.scriptMemoryLimitBytes == sc::kDefaultMemoryLimit),
      "no project restores the default limits");

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
  engine::shutdown();
  if (!bootstrap_headless(plain)) {
    g_tests.fail("the project without limits bootstraps headless");
    return;
  }
  g_tests.check((sc::get_instruction_limit() == sc::kDefaultInstructionLimit) &&
                    (sc::get_memory_limit() == sc::kDefaultMemoryLimit),
                "the next run, of a project setting none, is back on the "
                "defaults");
  engine::shutdown();
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

int main() {
  const fs::path root = fs::temp_directory_path() / "engine_project_open_test";
  std::error_code ec{};
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
  g_tests.check(engine::core::initialize_logging(), "initialize logging");

  test_opens(root);
  test_refusals(root);
  test_bundled_sample();
  engine::core::shutdown_logging();
  test_bootstrap(root);
  test_script_limits(root);
  test_packages(root);

  fs::remove_all(root, ec);
  return g_tests.finish("project_open");
}
