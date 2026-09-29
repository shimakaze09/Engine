// Declares the editor's helpers for files it creates in the project's
// per-user data directory (engine/core/project_data.h): the subdirectory a
// kind of file lives in (Screenshots/, Recordings/), created on first use,
// and a timestamped file name that never replaces an existing file, as
// Unreal names its screenshots and Unity's Recorder its takes.

#pragma once

#include <cstddef>
#include <ctime>

namespace engine::editor {

/// Whether a file exists at `path`; the seam next_timestamped_path takes
/// so tests can stage collisions.
using ProjectFileExistsFn = bool (*)(const char *path, void *userData) noexcept;

/// The file-system test the editor passes as ProjectFileExistsFn.
bool project_file_exists(const char *path, void *userData) noexcept;

/// The local time now, for next_timestamped_path.
std::tm local_time_now() noexcept;

/// Writes `<directory>/<stem>-YYYYMMDD-HHMMSS<extension>` for `time` into
/// `out`, or, when that file exists, the first free of `-2` to `-99`
/// before the extension. False, with `out` emptied, when every name is
/// taken or the path does not fit `capacity` whole.
bool next_timestamped_path(const char *directory, const char *stem,
                           const char *extension, const std::tm &time,
                           ProjectFileExistsFn exists, void *userData,
                           char *out, std::size_t capacity) noexcept;

/// Writes `<project data directory>/<name>` into `out` and creates it when
/// missing. False, logged, when no project is open, the path does not fit
/// `capacity` whole, or the directory cannot be created.
bool project_data_subdirectory(const char *name, char *out,
                               std::size_t capacity) noexcept;

} // namespace engine::editor
