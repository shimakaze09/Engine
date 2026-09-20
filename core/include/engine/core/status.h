// The failure vocabulary every tier shares: one category enum for what
// went wrong and a two-word Status that carries it, usable on noexcept
// hot paths because it allocates nothing and formats nothing. A caller
// tests it like a bool; the category is there for the one who needs to
// tell a full table from a missing file.

#pragma once

#include <cstdint>

namespace engine::core {

/// What went wrong, in terms a caller can act on.
enum class FailureKind : std::uint8_t {
  Ok = 0,
  /// The caller passed something the operation cannot take as given.
  InvalidArgument,
  /// The named thing does not exist or is no longer alive.
  NotFound,
  /// The operation is not available on this platform, build or object.
  Unsupported,
  /// A fixed-capacity table, queue or budget is full.
  CapacityExhausted,
  /// The data read does not fit its own format.
  DataMalformed,
  /// A filesystem or device operation failed.
  IoFailed,
  /// Failed now, may succeed later without a change from the caller.
  Transient,
  /// The operation was called in a state its contract forbids.
  InvariantViolated,
  /// Completed, but with a fidelity the caller should know about.
  Degraded,
};

/// Stable lower-case name of a category, for diagnostics.
constexpr const char *failure_kind_name(FailureKind kind) noexcept {
  switch (kind) {
  case FailureKind::Ok:
    return "ok";
  case FailureKind::InvalidArgument:
    return "invalid_argument";
  case FailureKind::NotFound:
    return "not_found";
  case FailureKind::Unsupported:
    return "unsupported";
  case FailureKind::CapacityExhausted:
    return "capacity_exhausted";
  case FailureKind::DataMalformed:
    return "data_malformed";
  case FailureKind::IoFailed:
    return "io_failed";
  case FailureKind::Transient:
    return "transient";
  case FailureKind::InvariantViolated:
    return "invariant_violated";
  case FailureKind::Degraded:
    return "degraded";
  }
  return "unknown";
}

/// The outcome of an operation: its category and a site-specific detail
/// code (0 when the category says it all; each API documents its own).
/// A Status must be looked at: dropping one silently is what this type
/// exists to prevent, so discard it with static_cast<void> on purpose.
struct [[nodiscard]] Status final {
  FailureKind kind = FailureKind::Ok;
  std::uint32_t detail = 0U;

  /// The success value.
  static constexpr Status ok() noexcept { return Status{}; }
  /// A failure of the given category.
  static constexpr Status fail(FailureKind failureKind,
                               std::uint32_t failureDetail = 0U) noexcept {
    return Status{failureKind, failureDetail};
  }

  /// True when the operation succeeded (Degraded is a success).
  constexpr bool succeeded() const noexcept {
    return (kind == FailureKind::Ok) || (kind == FailureKind::Degraded);
  }
  /// Lets a status stand in a condition, so `if (!op())` reads as before.
  constexpr explicit operator bool() const noexcept { return succeeded(); }

  friend constexpr bool operator==(const Status &, const Status &) = default;
};

} // namespace engine::core
