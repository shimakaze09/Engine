// Implements create_project: copies a project template into a hidden
// staging directory beside the destination, file by file through the
// durable atomic writer, adds a new .project document, and renames the
// stage into place as the one commit, as a save stages a sibling file.

#include "engine/project.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include "engine/content/asset_identity.h"
#include "engine/content/project_document.h"
#include "engine/core/atomic_file.h"
#include "engine/core/file_read.h"
#include "engine/core/logging.h"
#include "engine/core/nothrow_buffer.h"

namespace engine {

namespace {

namespace fs = std::filesystem;

constexpr const char *kLogChannel = "project";
constexpr const char *kStartupScene = "assets/main.scene";
constexpr const char *kMainScript = "assets/main.lua";

/// Logs why `name` was not created and returns the refusal.
std::unexpected<ProjectCreateFailureKind>
refuse(const char *name, ProjectCreateFailureKind kind,
       const std::string &detail) noexcept {
  char message[768] = {};
  std::snprintf(
      message, sizeof(message), "cannot create project '%.64s': %s%s%.500s",
      (name != nullptr) ? name : "", project_create_failure_text(kind),
      detail.empty() ? "" : ": ", detail.c_str());
  core::log_message(core::LogLevel::Error, kLogChannel, message);
  return std::unexpected(kind);
}

/// Removes the staging directory and everything in it. A failure leaves a
/// hidden directory behind, never anything at the project's own path, so
/// it is worth a warning and nothing more.
void remove_stage(const fs::path &stage) noexcept {
  std::error_code ec{};
  fs::remove_all(stage, ec);
  if (ec) {
    char message[640] = {};
    std::snprintf(message, sizeof(message),
                  "could not remove the staging directory %.500s",
                  stage.string().c_str());
    core::log_message(core::LogLevel::Warning, kLogChannel, message);
  }
}

/// Copies every directory and file of `from` into `stage`, each file
/// through the durable atomic writer. Fills `*kind` and `*detail` and
/// returns false at the first thing that cannot be copied: a file too
/// large or unreadable, something that is neither a file nor a directory,
/// or a .project document of the template's own (the project would hold
/// two).
bool copy_template(const fs::path &from, const fs::path &stage,
                   core::NothrowBuffer<char> *buffer,
                   ProjectCreateFailureKind *kind,
                   std::string *detail) noexcept {
  std::error_code ec{};
  fs::recursive_directory_iterator it(from, ec);
  for (; !ec && (it != fs::recursive_directory_iterator()); it.increment(ec)) {
    const fs::path source = it->path();
    const fs::path relative = source.lexically_relative(from);
    const fs::path target = stage / relative;
    std::error_code typeEc{};
    if (it->is_directory(typeEc) && !typeEc) {
      if (!core::create_directories_durably(target.string().c_str())) {
        *kind = ProjectCreateFailureKind::WriteFailed;
        *detail = target.string();
        return false;
      }
      continue;
    }
    if (!it->is_regular_file(typeEc) || typeEc ||
        (!relative.has_parent_path() &&
         (relative.extension() == content::kProjectFileExtension))) {
      *kind = ProjectCreateFailureKind::TemplateUnusable;
      *detail = source.string();
      return false;
    }
    std::size_t size = 0U;
    if (core::read_whole_file(source.string().c_str(), buffer->data(),
                              buffer->size(),
                              &size) != core::FileReadResult::Ok) {
      *kind = ProjectCreateFailureKind::TemplateUnusable;
      *detail = source.string();
      return false;
    }
    if (!core::atomic_write_file(target.string().c_str(), buffer->data(),
                                 size)) {
      *kind = ProjectCreateFailureKind::WriteFailed;
      *detail = target.string();
      return false;
    }
  }
  if (ec) {
    *kind = ProjectCreateFailureKind::TemplateUnusable;
    *detail = from.string();
    return false;
  }
  return true;
}

/// The new project's document: `name`, a fresh GUID, the template's
/// startup scene, and its main script when it has one.
content::ProjectDocument new_document(const char *name,
                                      bool hasMainScript) noexcept {
  content::ProjectDocument document{};
  std::snprintf(document.name, sizeof(document.name), "%s", name);
  std::snprintf(document.version, sizeof(document.version), "0.1.0");
  document.guid = content::generate_asset_guid();
  std::snprintf(document.contentRoot, sizeof(document.contentRoot), "%s",
                content::kProjectContentMount);
  std::snprintf(document.cacheRoot, sizeof(document.cacheRoot), ".cache");
  std::snprintf(document.scenes[0], sizeof(document.scenes[0]), "%s",
                kStartupScene);
  document.sceneCount = 1U;
  std::snprintf(document.startupScene, sizeof(document.startupScene), "%s",
                kStartupScene);
  if (hasMainScript) {
    std::snprintf(document.mainScript, sizeof(document.mainScript), "%s",
                  kMainScript);
  }
  return document;
}

} // namespace

std::expected<void, ProjectCreateFailureKind>
create_project(const char *location, const char *name,
               const char *templateDirectory, char *outProjectFile,
               std::size_t capacity) noexcept {
  if ((location == nullptr) || (location[0] == '\0') || (name == nullptr) ||
      (templateDirectory == nullptr) || (templateDirectory[0] == '\0') ||
      (outProjectFile == nullptr) || (capacity == 0U)) {
    return refuse(name, ProjectCreateFailureKind::InvalidArgument, "");
  }
  outProjectFile[0] = '\0';

  // The name is the document's own, so it passes the rules the document's
  // reader enforces or it is refused here, before anything is written.
  const std::size_t nameLength = std::strlen(name);
  if (nameLength >= content::kProjectNameCapacity) {
    return refuse(name, ProjectCreateFailureKind::InvalidName,
                  "it is longer than a project name can be");
  }
  const fs::path templateRoot(templateDirectory);
  std::error_code ec{};
  const bool hasMainScript =
      fs::is_regular_file(templateRoot / kMainScript, ec);
  content::ProjectDocument document = new_document(name, hasMainScript);
  const auto valid = content::validate_project_document(document);
  if (!valid.has_value()) {
    return refuse(name,
                  (std::strcmp(valid.error().field, "identity.name") == 0)
                      ? ProjectCreateFailureKind::InvalidName
                      : ProjectCreateFailureKind::InvalidArgument,
                  valid.error().reason);
  }

  ec.clear();
  if (!fs::is_directory(templateRoot, ec) || ec ||
      !fs::is_regular_file(templateRoot / kStartupScene, ec) || ec) {
    return refuse(name, ProjectCreateFailureKind::TemplateUnusable,
                  std::string(templateDirectory) +
                      " is not a directory holding " + kStartupScene);
  }
  const fs::path parent = fs::absolute(fs::path(location), ec);
  if (ec || !fs::is_directory(parent, ec) || ec) {
    return refuse(name, ProjectCreateFailureKind::LocationMissing, location);
  }
  const fs::path destination = (parent / name).lexically_normal();
  const fs::path stage =
      (parent / (std::string(".") + name + ".creating")).lexically_normal();
  const fs::path stagedDocument =
      stage / (std::string(name) + content::kProjectFileExtension);
  const std::string finalDocument =
      (destination / (std::string(name) + content::kProjectFileExtension))
          .generic_string();
  if ((finalDocument.size() >= capacity) ||
      (finalDocument.size() >= kProjectOsPathCapacity)) {
    return refuse(name, ProjectCreateFailureKind::PathTooLong, finalDocument);
  }
  // Anything there but "nothing" (a file, a directory, a dangling link,
  // or a path that cannot be examined) is refused.
  ec.clear();
  if (fs::symlink_status(destination, ec).type() != fs::file_type::not_found) {
    return refuse(name, ProjectCreateFailureKind::AlreadyExists,
                  destination.string());
  }

  core::NothrowBuffer<char> buffer{};
  if (!buffer.allocate(kMaxProjectTemplateFileBytes + 1U)) {
    return refuse(name, ProjectCreateFailureKind::WriteFailed,
                  "out of memory for the copy buffer");
  }

  // The stage's name is this function's own, so one already there is an
  // interrupted earlier attempt's, never an author's directory.
  remove_stage(stage);
  ProjectCreateFailureKind kind = ProjectCreateFailureKind::WriteFailed;
  std::string detail{};
  if (!core::create_directories_durably(stage.string().c_str())) {
    remove_stage(stage);
    return refuse(name, ProjectCreateFailureKind::WriteFailed, stage.string());
  }
  if (!copy_template(templateRoot, stage, &buffer, &kind, &detail)) {
    remove_stage(stage);
    return refuse(name, kind, detail);
  }
  if (!content::write_project_document(stagedDocument.string().c_str(),
                                       document)) {
    remove_stage(stage);
    return refuse(name, ProjectCreateFailureKind::WriteFailed,
                  stagedDocument.string());
  }
  ec.clear();
  fs::rename(stage, destination, ec);
  if (ec) {
    remove_stage(stage);
    return refuse(name, ProjectCreateFailureKind::WriteFailed,
                  "could not move the new project into place at " +
                      destination.string());
  }

  std::memcpy(outProjectFile, finalDocument.c_str(), finalDocument.size() + 1U);
  char message[512] = {};
  std::snprintf(message, sizeof(message), "created project '%s' at %.400s",
                name, destination.string().c_str());
  core::log_message(core::LogLevel::Info, kLogChannel, message);
  return {};
}

const char *
project_create_failure_text(ProjectCreateFailureKind kind) noexcept {
  switch (kind) {
  case ProjectCreateFailureKind::InvalidArgument:
    return "a required argument is missing";
  case ProjectCreateFailureKind::InvalidName:
    return "the name cannot be a project's name";
  case ProjectCreateFailureKind::LocationMissing:
    return "the location is not a folder";
  case ProjectCreateFailureKind::AlreadyExists:
    return "something with that name is already there";
  case ProjectCreateFailureKind::TemplateUnusable:
    return "the project template cannot be used";
  case ProjectCreateFailureKind::PathTooLong:
    return "the project's path is too long";
  case ProjectCreateFailureKind::WriteFailed:
    return "the project could not be written";
  }
  return "the project could not be created";
}

} // namespace engine
