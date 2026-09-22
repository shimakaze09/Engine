// Implements install_crash_report and the signal-safe report writer.
//
// The whole file is written to the constraint that its output path runs
// inside a signal handler: no allocation, no locks, no stdio, no
// snprintf, no locale. Text goes out through one raw write per fragment,
// integers through a local digit buffer, and every published value is
// read from a relaxed atomic that the handler only ever reads.

#include "engine/core/crash_report.h"

#include "engine/core/engine_version.h"
#include "engine/core/job_system.h"
#include "engine/core/logging.h"

#include <atomic>
#include <cstddef>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace engine::core {

namespace {

// Published by the engine, read by the handler. Relaxed throughout: a
// handler runs on the faulting thread, so it observes that thread's own
// stores, and a value from a neighbouring thread being one stage stale is
// worth far less than any synchronisation that could block here.
std::atomic<const char *const *> g_stageNames{nullptr};
std::atomic<std::uint32_t> g_stageCount{0U};
std::atomic<std::uint32_t> g_stage{0U};
std::atomic<bool> g_installed{false};

// One report per process. A fault inside the handler -- a corrupt stage
// table, a second thread faulting at the same moment -- must not loop
// back into it and write forever.
std::atomic<bool> g_reported{false};

#if defined(_WIN32)
LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;
#else
// The signals worth a report: each ends the process by default, and each
// can be reached by a bug rather than only by a deliberate kill.
constexpr int kHandledSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
constexpr std::size_t kHandledSignalCount =
    sizeof(kHandledSignals) / sizeof(kHandledSignals[0]);
struct sigaction g_previousActions[kHandledSignalCount] = {};
#endif

/// One raw write, retried on a partial write or an interrupted call. The
/// only output primitive in this file.
void write_all(int fileDescriptor, const char *text,
               std::size_t length) noexcept {
  std::size_t written = 0U;
  while (written < length) {
#if defined(_WIN32)
    const int result =
        ::_write(fileDescriptor, text + written,
                 static_cast<unsigned int>(length - written));
#else
    const ssize_t result =
        ::write(fileDescriptor, text + written, length - written);
#endif
    if (result <= 0) {
#if !defined(_WIN32)
      if ((result < 0) && (errno == EINTR)) {
        continue;
      }
#endif
      // Nothing useful is left to do about a closed or failing stderr
      // from inside a handler; stopping beats spinning.
      return;
    }
    written += static_cast<std::size_t>(result);
  }
}

/// Length of a null-terminated string, bounded so a corrupt or
/// unterminated pointer cannot walk the address space and fault inside
/// the handler.
std::size_t bounded_length(const char *text, std::size_t limit) noexcept {
  std::size_t length = 0U;
  while ((length < limit) && (text[length] != '\0')) {
    ++length;
  }
  return length;
}

void write_text(int fileDescriptor, const char *text) noexcept {
  if (text == nullptr) {
    return;
  }
  write_all(fileDescriptor, text, bounded_length(text, 4096U));
}

/// Unsigned decimal, written without the C library: a fixed buffer filled
/// from the back. 20 digits is the widest a 64-bit value can be.
void write_u64(int fileDescriptor, std::uint64_t value) noexcept {
  char digits[20] = {};
  std::size_t index = sizeof(digits);
  do {
    --index;
    digits[index] = static_cast<char>('0' + static_cast<char>(value % 10U));
    value /= 10U;
  } while ((value != 0U) && (index > 0U));
  write_all(fileDescriptor, digits + index, sizeof(digits) - index);
}

/// The registered name for the current stage, or a fixed string when no
/// table is registered or the index is out of range. Never dereferences
/// past the registered count.
const char *current_stage_name() noexcept {
  const char *const *names = g_stageNames.load(std::memory_order_relaxed);
  const std::uint32_t count = g_stageCount.load(std::memory_order_relaxed);
  const std::uint32_t stage = g_stage.load(std::memory_order_relaxed);
  if ((names == nullptr) || (count == 0U) || (stage >= count)) {
    return "unknown";
  }
  const char *name = names[stage];
  return (name != nullptr) ? name : "unknown";
}

} // namespace

void write_crash_report(int fileDescriptor, const char *reason) noexcept {
  write_text(fileDescriptor, "\nENGINE CRASH reason=");
  write_text(fileDescriptor, (reason != nullptr) ? reason : "unknown");
  write_text(fileDescriptor, " build=");
  write_text(fileDescriptor, engine_build_id());
  write_text(fileDescriptor, " frame=");
  // Whatever the pipeline last told logging. A relaxed load of a
  // lock-free atomic, which is all a handler may do.
  write_u64(fileDescriptor,
            static_cast<std::uint64_t>(log_current_frame_index()));
  write_text(fileDescriptor, " stage=");
  write_text(fileDescriptor, current_stage_name());
  write_text(fileDescriptor, " thread=");
  write_u64(fileDescriptor,
            static_cast<std::uint64_t>(current_thread_index()));
  write_text(fileDescriptor, "\n");
}

void set_crash_stage_table(const char *const *names,
                           std::uint32_t count) noexcept {
  // Count first, then the pointer: a handler that reads between the two
  // stores sees either no table or the old count against the new table,
  // and the range check in current_stage_name covers both.
  g_stageCount.store((names != nullptr) ? count : 0U,
                     std::memory_order_relaxed);
  g_stageNames.store(names, std::memory_order_relaxed);
}

void set_crash_stage(std::uint32_t stage) noexcept {
  g_stage.store(stage, std::memory_order_relaxed);
}

namespace {

#if defined(_WIN32)

/// The name for the codes worth distinguishing; anything else reports its
/// number, which is why the caller never formats.
const char *exception_name(DWORD code) noexcept {
  switch (code) {
  case EXCEPTION_ACCESS_VIOLATION:
    return "access-violation";
  case EXCEPTION_ILLEGAL_INSTRUCTION:
    return "illegal-instruction";
  case EXCEPTION_INT_DIVIDE_BY_ZERO:
    return "integer-divide-by-zero";
  case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    return "float-divide-by-zero";
  case EXCEPTION_STACK_OVERFLOW:
    return "stack-overflow";
  default:
    return "exception";
  }
}

LONG WINAPI crash_filter(EXCEPTION_POINTERS *info) noexcept {
  bool expected = false;
  if (g_reported.compare_exchange_strong(expected, true,
                                         std::memory_order_relaxed)) {
    const DWORD code = (info != nullptr)
                           ? info->ExceptionRecord->ExceptionCode
                           : 0U;
    write_crash_report(2, exception_name(code));
  }
  // Hand it on: whatever was going to happen -- the previous filter, the
  // debugger, the default crash dialog -- still happens.
  return (g_previousFilter != nullptr) ? g_previousFilter(info)
                                       : EXCEPTION_CONTINUE_SEARCH;
}

#else

const char *signal_name(int signalNumber) noexcept {
  switch (signalNumber) {
  case SIGSEGV:
    return "SIGSEGV";
  case SIGBUS:
    return "SIGBUS";
  case SIGILL:
    return "SIGILL";
  case SIGFPE:
    return "SIGFPE";
  case SIGABRT:
    return "SIGABRT";
  default:
    return "signal";
  }
}

/// Writes the report, restores the previous disposition, and re-raises, so
/// the process dies of what actually killed it: the exit status still
/// names the signal, a debugger still catches it, and a sanitizer's own
/// handler still runs if it had one installed.
void crash_handler(int signalNumber) noexcept {
  bool expected = false;
  if (g_reported.compare_exchange_strong(expected, true,
                                         std::memory_order_relaxed)) {
    write_crash_report(2, signal_name(signalNumber));
  }

  for (std::size_t i = 0U; i < kHandledSignalCount; ++i) {
    if (kHandledSignals[i] == signalNumber) {
      static_cast<void>(
          ::sigaction(signalNumber, &g_previousActions[i], nullptr));
      break;
    }
  }
  static_cast<void>(::raise(signalNumber));
}

#endif

} // namespace

bool install_crash_report() noexcept {
  bool expected = false;
  if (!g_installed.compare_exchange_strong(expected, true,
                                           std::memory_order_relaxed)) {
    return true;
  }

#if defined(_WIN32)
  g_previousFilter = ::SetUnhandledExceptionFilter(&crash_filter);
  return true;
#else
  struct sigaction action = {};
  action.sa_handler = &crash_handler;
  static_cast<void>(::sigemptyset(&action.sa_mask));
  // SA_NODEFER is deliberately absent: the signal stays blocked while the
  // handler runs, so a second fault inside it cannot re-enter. The
  // re-raise happens after the previous disposition is restored, and the
  // kernel delivers it when the handler returns.
  action.sa_flags = SA_RESTART;

  bool allInstalled = true;
  for (std::size_t i = 0U; i < kHandledSignalCount; ++i) {
    if (::sigaction(kHandledSignals[i], &action, &g_previousActions[i]) != 0) {
      allInstalled = false;
    }
  }
  if (!allInstalled) {
    shutdown_crash_report();
  }
  return allInstalled;
#endif
}

void shutdown_crash_report() noexcept {
  bool expected = true;
  if (!g_installed.compare_exchange_strong(expected, false,
                                           std::memory_order_relaxed)) {
    return;
  }

#if defined(_WIN32)
  static_cast<void>(::SetUnhandledExceptionFilter(g_previousFilter));
  g_previousFilter = nullptr;
#else
  for (std::size_t i = 0U; i < kHandledSignalCount; ++i) {
    static_cast<void>(
        ::sigaction(kHandledSignals[i], &g_previousActions[i], nullptr));
  }
#endif
}

} // namespace engine::core
