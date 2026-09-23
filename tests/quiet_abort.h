// Keeps a test helper's deliberate abort from blocking a headless run on
// Windows. The debug C runtime's abort() opens an "abort() has been called"
// dialog and waits for a click that never comes, so a helper that is meant
// to die instead hangs until ctest's timeout; Windows Error Reporting can
// do the same. Every helper whose purpose is to abort calls this first.
// Elsewhere it does nothing.

#pragma once

#if defined(_WIN32)
#include <crtdbg.h>
#include <cstdlib>
#endif

namespace engine::tests {

/// Sends abort's message and the debug runtime's reports to stderr instead
/// of a dialog, and skips the fault report, so the process just ends.
inline void quiet_abort_dialogs() noexcept {
#if defined(_WIN32)
  static_cast<void>(
      _set_abort_behavior(0U, _WRITE_ABORT_MSG | _CALL_REPORTFAULT));
  static_cast<void>(_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE));
  static_cast<void>(_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR));
  static_cast<void>(_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE));
  static_cast<void>(_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR));
#endif
}

} // namespace engine::tests
