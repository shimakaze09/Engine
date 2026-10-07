// Implements opening and switching projects: open_project finds the
// .project document, reads it through content's codec, checks that what it
// names exists on disk, and points an EngineConfig at the result;
// configure_without_project points one at none; the switch handoff holds
// the next project a run asked for until the executable's loop takes it.

#include "engine/project.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/core/vfs.h"

namespace engine {

namespace {

constexpr const char *kLogChannel = "project";

/// The switch a run asked for, held across the engine's shutdown until the
/// executable's loop takes it. Main thread only.
struct PendingSwitch final {
  bool pending = false;
  char path[kProjectOsPathCapacity] = {};
};

PendingSwitch g_switch{};

/// Logs the refusal and returns it.
std::unexpected<ProjectOpenFailure>
refuse(const char *path, ProjectOpenFailureKind kind, const char *detail,
       const content::ProjectReadFailure &document = {}) noexcept {
  char message[768] = {};
  std::snprintf(message, sizeof(message),
                "cannot open project '%.300s': %s%s%.300s", path,
                project_open_failure_text(kind),
                ((detail != nullptr) && (detail[0] != '\0')) ? ": " : "",
                (detail != nullptr) ? detail : "");
  core::log_message(core::LogLevel::Error, kLogChannel, message);
  ProjectOpenFailure failure{};
  failure.kind = kind;
  failure.document = document;
  return std::unexpected(failure);
}

/// True for a regular file named "<something>.project".
bool is_project_file(const std::filesystem::path &path) noexcept {
  std::error_code ec{};
  return (path.extension() == content::kProjectFileExtension) &&
         (path.stem().string().size() > 0U) &&
         std::filesystem::is_regular_file(path, ec) && !ec;
}

/// Copies the generic ('/'-separated) form of `path` into `out` whole;
/// false when it does not fit.
bool store_path(const std::filesystem::path &path, char *out,
                std::size_t capacity) noexcept {
  const std::string text = path.generic_string();
  if (text.size() >= capacity) {
    return false;
  }
  std::memcpy(out, text.c_str(), text.size() + 1U);
  return true;
}

/// The OS path of a document's virtual content path ("assets/x" under
/// `contentRoot`); empty when the path is not under the content mount.
std::filesystem::path content_os_path(const std::filesystem::path &contentRoot,
                                      const char *virtualPath) noexcept {
  const std::size_t mountLength = std::strlen(content::kProjectContentMount);
  if ((std::strncmp(virtualPath, content::kProjectContentMount,
                    mountLength) != 0) ||
      (virtualPath[mountLength] != '/')) {
    return {};
  }
  return contentRoot / (virtualPath + mountLength + 1U);
}

} // namespace

std::expected<void, ProjectOpenFailure>
open_project(const char *path, ProjectStorage *storage,
             EngineConfig *config) noexcept {
  if ((path == nullptr) || (path[0] == '\0') || (storage == nullptr) ||
      (config == nullptr)) {
    return refuse((path != nullptr) ? path : "",
                  ProjectOpenFailureKind::InvalidPath, "");
  }

  // Find the document: the path itself, or the only one in the directory.
  std::error_code ec{};
  std::filesystem::path projectFile{};
  const std::filesystem::path given(path);
  if (std::filesystem::is_directory(given, ec) && !ec) {
    std::size_t found = 0U;
    std::string names{};
    for (const auto &entry : std::filesystem::directory_iterator(given, ec)) {
      if (is_project_file(entry.path())) {
        ++found;
        projectFile = entry.path();
        names += (found == 1U) ? "" : ", ";
        names += entry.path().filename().string();
      }
    }
    if (ec || (found == 0U)) {
      return refuse(path, ProjectOpenFailureKind::NotFound,
                    "the directory holds no .project document");
    }
    if (found > 1U) {
      return refuse(path, ProjectOpenFailureKind::Ambiguous, names.c_str());
    }
  } else if (is_project_file(given)) {
    projectFile = given;
  } else {
    return refuse(path, ProjectOpenFailureKind::NotFound,
                  "not a directory or a .project document");
  }

  // Staged whole, so a refusal below leaves the caller's storage as it was.
  // About 18 KB, fine on a cold path's stack.
  ProjectStorage stagedStorage{};
  ProjectStorage *staged = &stagedStorage;

  const std::filesystem::path absoluteFile =
      std::filesystem::absolute(projectFile, ec).lexically_normal();
  if (ec) {
    return refuse(path, ProjectOpenFailureKind::NotFound,
                  "cannot resolve the document's path");
  }
  const std::string fileText = absoluteFile.string();
  const auto document =
      content::read_project_document(fileText.c_str(), &staged->document);
  if (!document.has_value()) {
    return refuse(path, ProjectOpenFailureKind::DocumentRefused,
                  document.error().reason, document.error());
  }
  const content::ProjectEngineVersion running =
      content::running_engine_version();
  if (staged->document.engine.set &&
      (content::compare_engine_versions(staged->document.engine, running) >
       0)) {
    char detail[160] = {};
    std::snprintf(detail, sizeof(detail),
                  "it was saved by engine %u.%u.%u; this is %u.%u.%u",
                  staged->document.engine.majorVersion,
                  staged->document.engine.minorVersion,
                  staged->document.engine.patchVersion, running.majorVersion,
                  running.minorVersion, running.patchVersion);
    return refuse(path, ProjectOpenFailureKind::NewerEngine, detail);
  }

  const std::filesystem::path directory = absoluteFile.parent_path();
  const std::filesystem::path contentRoot =
      (directory / staged->document.contentRoot).lexically_normal();
  if (!store_path(absoluteFile, staged->projectFile,
                  sizeof(staged->projectFile)) ||
      !store_path(directory, staged->projectDirectory,
                  sizeof(staged->projectDirectory)) ||
      !store_path(contentRoot, staged->contentRoot,
                  sizeof(staged->contentRoot))) {
    return refuse(path, ProjectOpenFailureKind::PathTooLong,
                  "move the project to a shorter path");
  }
  if (!std::filesystem::is_directory(contentRoot, ec) || ec) {
    return refuse(path, ProjectOpenFailureKind::ContentRootMissing,
                  staged->contentRoot);
  }

  const std::filesystem::path scene =
      content_os_path(contentRoot, staged->document.startupScene);
  if (scene.empty() || !std::filesystem::is_regular_file(scene, ec) || ec) {
    return refuse(path, ProjectOpenFailureKind::StartupSceneMissing,
                  staged->document.startupScene);
  }
  if (staged->document.mainScript[0] != '\0') {
    const std::filesystem::path script =
        content_os_path(contentRoot, staged->document.mainScript);
    if (script.empty() || !std::filesystem::is_regular_file(script, ec) ||
        ec) {
      return refuse(path, ProjectOpenFailureKind::MainScriptMissing,
                    staged->document.mainScript);
    }
  }

  static_assert(content::kMaxProjectPackages <= kMaxPackageMounts);
  for (std::size_t i = 0U; i < staged->document.packageCount; ++i) {
    const content::ProjectPackage &package = staged->document.packages[i];
    const std::filesystem::path root =
        (directory / package.source).lexically_normal();
    std::snprintf(staged->packageMounts[i], sizeof(staged->packageMounts[i]),
                  "%s/%s", content::kProjectPackagesMount, package.name);
    if (!store_path(root, staged->packageRoots[i],
                    sizeof(staged->packageRoots[i]))) {
      return refuse(path, ProjectOpenFailureKind::PathTooLong,
                    "move the project to a shorter path");
    }
    if (!std::filesystem::is_directory(root, ec) || ec) {
      return refuse(path, ProjectOpenFailureKind::PackageMissing,
                    staged->packageRoots[i]);
    }
  }

  *storage = *staged;
  for (std::size_t i = 0U; i < storage->document.packageCount; ++i) {
    storage->packages[i] =
        ContentMount{storage->packageMounts[i], storage->packageRoots[i]};
  }
  config->packages =
      (storage->document.packageCount > 0U) ? storage->packages : nullptr;
  config->packageCount = storage->document.packageCount;
  config->assetMount = content::kProjectContentMount;
  config->assetRoot = storage->contentRoot;
  config->projectFile = storage->projectFile;
  config->editorAssetRoot = storage->contentRoot;
  config->editorScenePath = storage->document.startupScene;
  config->mainScriptPath = storage->document.mainScript;
  const ScriptLimits limits =
      project_script_limits(storage->document.scriptLimits);
  config->scriptInstructionLimit = limits.instructionLimit;
  config->scriptMemoryLimitBytes = limits.memoryLimitBytes;
  config->collisionLayers = storage->document.collisionLayers;
  const content::ProjectSaveSettings &saves = storage->document.saveSettings;
  config->saveSlotLimitBytes =
      saves.maxSlotMiBSet
          ? static_cast<std::size_t>(saves.maxSlotMiB) * 1024U * 1024U
          : runtime::kDefaultSaveSlotLimitBytes;
  config->core.projectGuid = storage->document.guid;

  char message[512] = {};
  std::snprintf(message, sizeof(message), "opened project '%s' from %.400s",
                storage->document.name, storage->projectFile);
  core::log_message(core::LogLevel::Info, kLogChannel, message);
  return {};
}

ScriptLimits
project_script_limits(const content::ProjectScriptLimits &limits) noexcept {
  ScriptLimits result{};
  if (limits.instructionLimitSet) {
    result.instructionLimit = static_cast<int>(limits.instructionLimit);
  }
  if (limits.memoryLimitSet) {
    result.memoryLimitBytes =
        static_cast<std::size_t>(limits.memoryLimitMiB) * 1024U * 1024U;
  }
  return result;
}

const char *project_open_failure_text(ProjectOpenFailureKind kind) noexcept {
  switch (kind) {
  case ProjectOpenFailureKind::InvalidPath:
    return "no project path given";
  case ProjectOpenFailureKind::NotFound:
    return "no project found";
  case ProjectOpenFailureKind::Ambiguous:
    return "more than one .project document";
  case ProjectOpenFailureKind::DocumentRefused:
    return "the project document was refused";
  case ProjectOpenFailureKind::ContentRootMissing:
    return "the content root is missing";
  case ProjectOpenFailureKind::StartupSceneMissing:
    return "the startup scene is missing";
  case ProjectOpenFailureKind::MainScriptMissing:
    return "the main script is missing";
  case ProjectOpenFailureKind::PackageMissing:
    return "a package the project depends on is missing";
  case ProjectOpenFailureKind::PathTooLong:
    return "a project path is too long";
  case ProjectOpenFailureKind::NewerEngine:
    return "the project was saved by a newer engine";
  }
  return "the project could not be opened";
}

void configure_without_project(EngineConfig *config) noexcept {
  if (config == nullptr) {
    return;
  }
  config->assetRoot = "";
  config->projectFile = "";
  config->editorAssetRoot = "";
  config->editorScenePath = "";
  config->mainScriptPath = "";
  config->packages = nullptr;
  config->packageCount = 0U;
  const ScriptLimits defaults{};
  config->scriptInstructionLimit = defaults.instructionLimit;
  config->scriptMemoryLimitBytes = defaults.memoryLimitBytes;
  config->collisionLayers = content::ProjectCollisionLayers{};
  config->saveSlotLimitBytes = runtime::kDefaultSaveSlotLimitBytes;
  config->core.projectGuid = core::AssetGuid{};
}

bool request_project_switch(const char *path) noexcept {
  if (path == nullptr) {
    return false;
  }
  const std::size_t length = std::strlen(path);
  if (length >= sizeof(g_switch.path)) {
    char message[160] = {};
    std::snprintf(message, sizeof(message),
                  "cannot switch to a project path of %zu characters; the "
                  "limit is %zu",
                  length, sizeof(g_switch.path) - 1U);
    core::log_message(core::LogLevel::Error, kLogChannel, message);
    return false;
  }
  std::memcpy(g_switch.path, path, length + 1U);
  g_switch.pending = true;
  core::request_platform_quit();
  return true;
}

bool take_project_switch(char *out, std::size_t capacity,
                         bool *toHub) noexcept {
  if ((out == nullptr) || (capacity == 0U)) {
    return false;
  }
  out[0] = '\0';
  const std::size_t length = std::strlen(g_switch.path);
  if (!g_switch.pending || (length >= capacity)) {
    return false;
  }
  std::memcpy(out, g_switch.path, length + 1U);
  if (toHub != nullptr) {
    *toHub = (length == 0U);
  }
  g_switch = PendingSwitch{};
  return true;
}

bool find_bundled_sample_project(char *out, std::size_t capacity) noexcept {
  if ((out == nullptr) || (capacity == 0U)) {
    return false;
  }
  out[0] = '\0';
  char appDir[512] = {};
  if (!core::platform_get_app_dir(appDir, sizeof(appDir))) {
    return false;
  }
  const std::size_t length = std::strlen(appDir);
  const bool slash = (length > 0U) && ((appDir[length - 1U] == '/') ||
                                       (appDir[length - 1U] == '\\'));
  const int written = std::snprintf(out, capacity, "%s%ssamples/island", appDir,
                                    slash ? "" : "/");
  if ((written <= 0) || (static_cast<std::size_t>(written) >= capacity) ||
      !core::os_directory_exists(out)) {
    out[0] = '\0';
    return false;
  }
  return true;
}

} // namespace engine
