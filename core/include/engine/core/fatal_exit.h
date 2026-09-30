// Ends the process on a fault the engine detected itself (a GPU device
// lost, a renderer backend that cannot continue) without losing the
// author's unsaved work. A signal handler cannot do this: serializing a
// scene allocates and writes files, which is not async-signal-safe. So a
// detected fatal first runs a registered recovery hook in normal context,
// then writes the same crash report a fault would, says what happened in
// an error box, flushes the log, and exits with a code of its own rather
// than abort().
//
// The hook is registered by the tier that knows what is worth saving (the
// runtime, which asks the editor); core only calls it.

#pragma once

#include <cstddef>

namespace engine::core {

/// Exit code of a process ended by a graphics-device fatal. Distinct from
/// every engine::ExitCode a run can return, which the runtime pins.
inline constexpr int kFatalDeviceExitCode = 4;

/// Called on the thread that met the fatal, in normal (not signal)
/// context, to save what can be saved. Writes a one-line note for the
/// user into `note` (empty when nothing was saved).
using FatalRecoveryHook = void (*)(char *note, std::size_t capacity) noexcept;

/// Registers the recovery hook (null clears it) and re-arms it, so the
/// next fatal runs it.
void set_fatal_recovery_hook(FatalRecoveryHook hook) noexcept;

/// Runs the registered hook at most once per registration, so a fault
/// inside the hook, or a second fatal on the way out, never re-enters it.
/// `note` receives the hook's note, or is emptied. True when the hook ran.
bool run_fatal_recovery_hook(char *note, std::size_t capacity) noexcept;

/// Ends the process after `reason`, a fatal the engine detected. Runs the
/// recovery hook, logs, writes the crash report to stderr, shows an error
/// box naming the reason, the hook's note and the log file (nothing is
/// shown headless), closes the log file, and exits with `exitCode`. A
/// second call while one is in progress exits at once.
[[noreturn]] void terminate_after_fatal(const char *reason,
                                        int exitCode) noexcept;

} // namespace engine::core
