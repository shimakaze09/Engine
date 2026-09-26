// Declares the per-project user data directory: where a running game keeps
// what belongs to one player of one project (its save slot, its rebound
// input map). It sits under the per-user platform save directory, in
// projects/<16 hex digits>, the digits a hash of the project's identity, so
// two projects on one machine never read or replace each other's files.
// Until projects carry a document of their own, the identity is the
// project's content root as an absolute path; a project GUID can take its
// place without changing the call sites.

#pragma once

#include <cstddef>

namespace engine::core {

/// Names the project whose data the directory below belongs to, by its
/// content root (a directory that exists). The root is resolved to an
/// absolute path first, so a relative root names the same project from
/// any working directory; on Windows the comparison ignores ASCII case and
/// separator style, as the file system does. False, with an Error logged
/// and the previous project cleared, when the root cannot be resolved.
bool set_project_data_root(const char *projectRoot) noexcept;

/// True while a project is named.
bool project_data_named() noexcept;

/// Forgets the project; project_data_dir refuses until one is named again.
/// Core shutdown and a failed core initialization call it, so no later run
/// inherits this run's project.
void clear_project_data_root() noexcept;

/// The current project's data directory, which may not exist yet. False
/// when no project is named (logged as an Error: writing to a shared
/// directory instead is the defect this directory exists to prevent), or
/// when the path does not fit.
bool project_data_dir(char *outBuffer, std::size_t bufferCapacity) noexcept;

} // namespace engine::core
