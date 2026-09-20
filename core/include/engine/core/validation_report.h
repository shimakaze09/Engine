// A fixed-capacity report of what a loader found wrong or doubtful in a
// document without refusing it: dangling references, missing files, and
// the like. Loaders fill it, the editor, the validate tool and CI read
// it, so a defect in authored content is named once in a shape every
// consumer understands instead of being rooted, dropped or logged as
// prose.

#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::core {

/// How serious one finding is: a Warning loads with a fallback, an Error
/// is content the loader could not honour.
enum class ValidationSeverity : std::uint8_t { Warning, Error };

/// One finding: a short stable code, the key or path it concerns, and
/// the entity it belongs to (0 when none).
struct ValidationEntry final {
  static constexpr std::size_t kMaxCode = 32U;
  static constexpr std::size_t kMaxKey = 192U;

  ValidationSeverity severity = ValidationSeverity::Warning;
  char code[kMaxCode] = {};
  char key[kMaxKey] = {};
  std::uint32_t entityPersistentId = 0U;
};

/// The findings of one load, oldest first. Past capacity the report only
/// counts what it dropped, so a document with thousands of faults still
/// reports the first ones exactly.
struct ValidationReport final {
  static constexpr std::size_t kMaxEntries = 64U;

  ValidationEntry entries[kMaxEntries] = {};
  std::size_t count = 0U;
  std::size_t dropped = 0U;

  /// Records one finding; false when the report is full (the finding is
  /// counted in `dropped`). Over-long codes and keys are cut to fit.
  bool add(ValidationSeverity severity, const char *code, const char *key,
           std::uint32_t entityPersistentId) noexcept;
  /// Findings of the given severity, including dropped ones only by
  /// omission: dropped findings are not counted here.
  std::size_t count_of(ValidationSeverity severity) const noexcept;
  /// True when nothing was found and nothing was dropped.
  bool clean() const noexcept { return (count == 0U) && (dropped == 0U); }
};

} // namespace engine::core
