// Implements the packer's child-process launch over each host's native
// process API: posix_spawn where one exists, CreateProcess on Windows.
// Neither path involves a command interpreter, so an argument is never
// re-parsed on its way to the child. The Windows path has to flatten the
// vector into the single command-line string that API takes, so it applies
// the quoting rule the C runtime's own argument parser is defined against,
// which round-trips the flattened string back to the vector given here.

#include "process_run.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstddef>
#include <vector>

#ifndef _WIN32
/// The environment the packer runs under, handed to the child unchanged so a
/// tool keeps the PATH, temporary directory and locale the cook started with.
extern "C" char **environ;
#endif

namespace {

#ifdef _WIN32

/// Quotes one argument for the command-line parser the C runtime applies to
/// a child's command line: a run of backslashes is doubled only where it
/// precedes a quote, and an embedded quote is escaped.
std::string quote_argument(const std::string &value) {
  const bool needsQuotes =
      value.empty() || (value.find_first_of(" \t\n\v\"") != std::string::npos);
  if (!needsQuotes) {
    return value;
  }

  std::string quoted = "\"";
  for (std::size_t i = 0U; i < value.size(); ++i) {
    std::size_t backslashes = 0U;
    while ((i < value.size()) && (value[i] == '\\')) {
      ++i;
      ++backslashes;
    }
    if (i == value.size()) {
      // Trailing backslashes precede the closing quote, so they double.
      quoted.append(backslashes * 2U, '\\');
      break;
    }
    if (value[i] == '"') {
      quoted.append((backslashes * 2U) + 1U, '\\');
    } else {
      quoted.append(backslashes, '\\');
    }
    quoted.push_back(value[i]);
  }
  quoted.push_back('"');
  return quoted;
}

#endif

} // namespace

ProcessResult run_process(const std::string &executable,
                          const std::vector<std::string> &arguments,
                          bool discardStandardError) {
  ProcessResult result{};

#ifdef _WIN32
  std::string commandLine = quote_argument(executable);
  for (const std::string &argument : arguments) {
    commandLine += " ";
    commandLine += quote_argument(argument);
  }
  // CreateProcess may write to the command line it is given, so it gets a
  // mutable copy rather than the string's own buffer.
  std::vector<char> mutableCommandLine(commandLine.begin(), commandLine.end());
  mutableCommandLine.push_back('\0');

  SECURITY_ATTRIBUTES inheritable{};
  inheritable.nLength = sizeof(inheritable);
  inheritable.bInheritHandle = TRUE;

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  HANDLE nullDevice = INVALID_HANDLE_VALUE;
  if (discardStandardError) {
    nullDevice = CreateFileA("NUL", GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                             OPEN_EXISTING, 0, nullptr);
  }
  if (nullDevice != INVALID_HANDLE_VALUE) {
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = nullDevice;
  }

  // The executable is named by the command line's first argument rather than
  // separately, so a caller may name a tool to be found on PATH; the quoting
  // above keeps a path containing spaces unambiguous to that lookup.
  PROCESS_INFORMATION process{};
  const BOOL started =
      CreateProcessA(nullptr, mutableCommandLine.data(), nullptr, nullptr,
                     TRUE, 0, nullptr, nullptr, &startup, &process);
  if (nullDevice != INVALID_HANDLE_VALUE) {
    CloseHandle(nullDevice);
  }
  if (started == FALSE) {
    return result;
  }

  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exitCode = 1UL;
  if (GetExitCodeProcess(process.hProcess, &exitCode) == FALSE) {
    exitCode = 1UL;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);

  result.launched = true;
  result.exitCode = static_cast<int>(exitCode);
  return result;
#else
  // posix_spawn takes a mutable argv; the strings outlive the call, and the
  // child receives copies, so pointing at the callers' buffers is safe.
  std::vector<char *> argv;
  argv.reserve(arguments.size() + 2U);
  argv.push_back(const_cast<char *>(executable.c_str()));
  for (const std::string &argument : arguments) {
    argv.push_back(const_cast<char *>(argument.c_str()));
  }
  argv.push_back(nullptr);

  posix_spawn_file_actions_t actions{};
  if (posix_spawn_file_actions_init(&actions) != 0) {
    return result;
  }
  if (discardStandardError &&
      (posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                        O_WRONLY, 0) != 0)) {
    posix_spawn_file_actions_destroy(&actions);
    return result;
  }

  // The PATH-searching spawn resolves a bare tool name against PATH; a name
  // carrying a separator is used as the path it spells.
  pid_t child = 0;
  const int spawned = posix_spawnp(&child, executable.c_str(), &actions,
                                   nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (spawned != 0) {
    return result;
  }

  // The child is running from here on, so the run counts as launched
  // whatever the wait reports; a wait that cannot resolve leaves the default
  // failing exit code in place.
  result.launched = true;
  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) {
      return result;
    }
  }

  // A child killed by a signal reports the shell's conventional 128 + signal
  // so callers see one nonzero failure code either way.
  result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status)
                                      : (128 + WTERMSIG(status));
  return result;
#endif
}
