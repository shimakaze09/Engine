// The engine's assertion, with behaviour that does not depend on NDEBUG.
//
// <cassert>'s assert compiles to nothing in a release build, which turns
// every bounds check written with it into undefined behaviour exactly
// where it was supposed to be caught -- the shipped configuration reads
// past the array and carries on with whatever it found. A check worth
// writing is worth keeping.
//
// ENGINE_ASSERT is for programmer error: a contract the caller broke,
// which no amount of retrying fixes and which has no correct value to
// return. It logs the file, line and condition at Fatal, which aborts, in
// every configuration. A deterministic abort naming the broken contract
// beats silent corruption in every case where both are possible.
//
// It is not for recoverable failure. A full table, a missing file, a
// malformed document and a dead entity are all things a caller can act
// on, and those return core::Status (status.h) instead. If you are
// reaching for an assertion on a path that has a sensible answer for the
// caller, the answer is the better design.

#pragma once

namespace engine::core {

/// Logs a broken contract at Fatal and aborts. Called by ENGINE_ASSERT;
/// not meant to be called directly.
///
/// Out of line so the macro costs one untaken branch at each use and does
/// not drag logging into every header that asserts, and [[noreturn]] so
/// the compiler knows the failing path does not come back -- a caller
/// returning a reference needs no unreachable dummy to satisfy it.
[[noreturn]] void assertion_failed(const char *condition, const char *file,
                                   int line) noexcept;

} // namespace engine::core

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define ENGINE_ASSERT(condition)                                             \
  do {                                                                       \
    if (!(condition)) {                                                      \
      ::engine::core::assertion_failed(#condition, __FILE__, __LINE__);      \
    }                                                                        \
  } while (false)
