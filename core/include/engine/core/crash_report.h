// The minimum a crash has to leave behind: which binary it was, how far
// into the run it got, what the engine was doing, and on which thread.
// Without those four, a stack-free crash in a redirected log is
// indistinguishable from any other crash and cannot be matched to a build.
//
// Everything here is written from a signal handler, so nothing in the
// write path may allocate, take a lock, format through the C library, or
// call anything else that is not async-signal-safe. The values are
// therefore published as plain integers and a registered table of names,
// and the handler turns them into text with a hand-rolled digit writer.
// That is also why there is no "message" to attach: a handler that
// formats is a handler that can deadlock against the allocator it
// interrupted.
//
// The frame index is not published here. The pipeline already tells
// logging which frame it is on, and the report reads that rather than
// keeping a second copy that could disagree with the log lines beside it.

#pragma once

#include <cstdint>

namespace engine::core {

/// Installs handlers for the faults that end a process -- on POSIX
/// SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGABRT; on Windows the unhandled
/// exception filter. Each writes the report to stderr and then lets the
/// fault continue to its default action, so the exit status and any
/// external crash reporting still see the original signal.
///
/// Idempotent: a second call is a no-op and returns true. False means the
/// platform refused an installation, and the previous disposition is left
/// in place.
bool install_crash_report() noexcept;

/// Restores the previous handlers. Safe to call without an install.
void shutdown_crash_report() noexcept;

/// Publishes the table of stage names the report prints, owned by the
/// caller and read from a signal handler, so it must have static storage
/// duration and outlive the process's last fault -- a literal array of
/// literals. `count` names how many entries `names` holds; an index at or
/// past it prints as "unknown" rather than reading past the end.
///
/// Registered rather than defined here because the stages belong to the
/// pipeline, and core cannot know them without depending upward.
void set_crash_stage_table(const char *const *names,
                           std::uint32_t count) noexcept;

/// Records the stage the engine is in. One relaxed store; call it as often
/// as the stage changes.
void set_crash_stage(std::uint32_t stage) noexcept;

/// Writes the report to the given file descriptor (or handle on Windows)
/// exactly as a fault would, for tests and for a caller that wants the
/// same line in its own diagnostics. Uses only the signal-safe write
/// path.
void write_crash_report(int fileDescriptor, const char *reason) noexcept;

} // namespace engine::core
