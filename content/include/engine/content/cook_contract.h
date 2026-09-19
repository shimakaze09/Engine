// Declares the cook contract shared by the asset packer (which writes cook
// stamps) and the runtime (which validates them before a load): the stamp
// text schema, the importer tool version, the stamp line limit and the
// rule a stamp-relative path must satisfy. One definition keeps the writer
// and the reader in agreement, so a bump on either side is a bump on both
// and a cooked tree from another tool is refused instead of silently
// loaded.

#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::content {

/// Layout version of the `.cookstamp` text: which key lines exist and
/// what they mean. Schema 3 carried the TOOL_VERSION, PLATFORM and OUTPUT
/// manifest lines with paths recorded exactly as the packer was invoked
/// (working-directory relative). Schema 4 records every DEP_HASH and
/// OUTPUT path relative to the stamp's own directory with `/` separators,
/// so a stamp certifies the same files from any working directory, and
/// an OUTPUT path must stay inside that directory. The one
/// exception is a DEP_HASH on another volume than the stamp, which has
/// no relative form — Windows drives share no root — and is recorded by
/// its normalized absolute path: still the same file from any working
/// directory, and a dependency is only read, never removed. A stamp
/// declaring a higher schema was written by a newer packer whose lines
/// this reader cannot interpret, so it certifies nothing here; a lower or
/// absent schema is a legacy stamp that recooks.
inline constexpr std::uint32_t kCookStampSchema = 4U;

/// Importer contract version baked into every cook stamp: bump whenever
/// the cooked output format or import semantics change, so existing
/// outputs recook once instead of silently keeping stale bytes, and the
/// runtime refuses a stamp from any other version until the tree is
/// recooked. Stamps written before this key existed read as version 0 in
/// the packer and always recook. Version 4 pairs with schema 4's
/// stamp-relative paths.
inline constexpr std::uint32_t kCookToolVersion = 4U;

/// Longest stamp line the writer emits and either reader accepts,
/// terminator included. A path that would not fit is refused at write
/// time and a line that does not fit at read time is a corrupt stamp;
/// neither side ever truncates a path.
inline constexpr std::size_t kMaxCookStampLineBytes = 1024U;

/// True when a schema-4 stamp path can be joined under the stamp's
/// directory without leaving it: non-empty, relative (no root, no drive),
/// and without a `.` or `..` segment. Both readers apply it to OUTPUT
/// paths before touching the file; the packer applies it to the paths it
/// records.
inline bool cook_stamp_path_is_contained(const char *path) noexcept {
  if ((path == nullptr) || (path[0] == '\0') || (path[0] == '/') ||
      (path[0] == '\\')) {
    return false;
  }
  if ((path[1] == ':') && (((path[0] >= 'A') && (path[0] <= 'Z')) ||
                           ((path[0] >= 'a') && (path[0] <= 'z')))) {
    return false;
  }
  const char *segment = path;
  for (const char *cursor = path;; ++cursor) {
    const bool atSeparator =
        (*cursor == '/') || (*cursor == '\\') || (*cursor == '\0');
    if (atSeparator) {
      const std::size_t length = static_cast<std::size_t>(cursor - segment);
      if ((length == 0U) ||
          ((length == 1U) && (segment[0] == '.')) ||
          ((length == 2U) && (segment[0] == '.') && (segment[1] == '.'))) {
        return false;
      }
      if (*cursor == '\0') {
        return true;
      }
      segment = cursor + 1;
    }
  }
}

} // namespace engine::content
