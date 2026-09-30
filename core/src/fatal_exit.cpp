// Implements the detected-fatal exit: one guarded recovery hook, then the
// crash report, the error box, the log flush and a plain exit.

#include "engine/core/fatal_exit.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "engine/core/crash_report.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"

namespace engine::core {

namespace {

std::atomic<FatalRecoveryHook> g_hook{nullptr};
// Set once the hook has been claimed; cleared when a hook is registered.
std::atomic<bool> g_hookClaimed{false};
std::atomic<bool> g_terminating{false};

} // namespace

void set_fatal_recovery_hook(FatalRecoveryHook hook) noexcept {
  g_hook.store(hook);
  g_hookClaimed.store(false);
}

bool run_fatal_recovery_hook(char *note, std::size_t capacity) noexcept {
  if ((note != nullptr) && (capacity > 0U)) {
    note[0] = '\0';
  }
  const FatalRecoveryHook hook = g_hook.load();
  if ((hook == nullptr) || g_hookClaimed.exchange(true)) {
    return false;
  }
  char scratch[1] = {};
  if ((note == nullptr) || (capacity == 0U)) {
    hook(scratch, sizeof(scratch));
  } else {
    hook(note, capacity);
  }
  return true;
}

void terminate_after_fatal(const char *reason, int exitCode) noexcept {
  if (g_terminating.exchange(true)) {
    std::_Exit(exitCode);
  }
  const char *why = (reason != nullptr) ? reason : "unrecoverable error";
  log_message(LogLevel::Error, "engine", why);

  char note[640] = {};
  static_cast<void>(run_fatal_recovery_hook(note, sizeof(note)));
  write_crash_report(2, why);

  char text[1400] = {};
  const char *logPath = log_file_path();
  std::snprintf(text, sizeof(text), "The engine has to close: %.300s%s%s%s%s",
                why, (note[0] != '\0') ? "\n\n" : "", note,
                (logPath[0] != '\0') ? "\n\nThe log is at:\n" : "", logPath);
  std::fprintf(stderr, "%s\n", text);
  platform_show_error_box("Engine", text);
  log_close_file();
  std::_Exit(exitCode);
}

} // namespace engine::core
