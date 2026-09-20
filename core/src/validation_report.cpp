// Implements the validation report's bounded append and counts.

#include "engine/core/validation_report.h"

#include <cstdio>

namespace engine::core {

bool ValidationReport::add(ValidationSeverity severity, const char *code,
                           const char *key,
                           std::uint32_t entityPersistentId) noexcept {
  if (count >= kMaxEntries) {
    ++dropped;
    return false;
  }
  ValidationEntry &entry = entries[count];
  entry.severity = severity;
  std::snprintf(entry.code, sizeof(entry.code), "%s",
                (code != nullptr) ? code : "");
  std::snprintf(entry.key, sizeof(entry.key), "%s",
                (key != nullptr) ? key : "");
  entry.entityPersistentId = entityPersistentId;
  ++count;
  return true;
}

std::size_t
ValidationReport::count_of(ValidationSeverity severity) const noexcept {
  std::size_t matching = 0U;
  for (std::size_t i = 0U; i < count; ++i) {
    if (entries[i].severity == severity) {
      ++matching;
    }
  }
  return matching;
}

} // namespace engine::core
