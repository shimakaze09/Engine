// Implements console behavior for the Engine core engine.

#include "engine/core/console.h"

#include "engine/core/cvar.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace engine::core {

namespace {

// ---- capacity constants ----
constexpr std::size_t kMaxCommands = 128U;
constexpr std::size_t kMaxArgs = 32U;
constexpr std::size_t kMaxOutputLines = 256U;
constexpr std::size_t kMaxLineLen = 256U;
constexpr std::size_t kMaxNameLen = 64U;
constexpr std::size_t kMaxDescLen = 128U;
constexpr std::size_t kMaxInputLen = 512U;

// ---- command registry ----
struct CommandEntry final {
  char name[kMaxNameLen] = {};
  char desc[kMaxDescLen] = {};
  ConsoleCommandFn fn = nullptr;
  void *userData = nullptr;
  bool used = false;
};

// ---- ring buffer for output ----
struct OutputLine final {
  char text[kMaxLineLen] = {};
};

bool g_initialized = false;
std::array<CommandEntry, kMaxCommands> g_commands{};
std::size_t g_commandCount = 0U;

std::array<OutputLine, kMaxOutputLines> g_outputBuf{};
std::size_t g_outputHead = 0U; // oldest entry
std::size_t g_outputCount = 0U;
std::mutex g_mutex{};

struct CommandInfoSnapshot final {
  char names[kMaxCommands][kMaxNameLen] = {};
  char descriptions[kMaxCommands][kMaxDescLen] = {};
};

thread_local CommandInfoSnapshot g_commandInfoSnapshot{};

bool register_command_unlocked(const char *name, ConsoleCommandFn fn,
                               void *userData,
                               const char *description) noexcept {
  if ((name == nullptr) || (fn == nullptr) || (description == nullptr)) {
    return false;
  }
  if (g_commandCount >= kMaxCommands) {
    return false;
  }
  for (std::size_t i = 0U; i < g_commandCount; ++i) {
    if (g_commands[i].used && std::strcmp(g_commands[i].name, name) == 0) {
      return false;
    }
  }

  CommandEntry &entry = g_commands[g_commandCount++];
  std::snprintf(entry.name, kMaxNameLen - 1U + 1U, "%s", name);
  std::snprintf(entry.desc, kMaxDescLen - 1U + 1U, "%s", description);
  entry.fn = fn;
  entry.userData = userData;
  entry.used = true;
  return true;
}

void print_unlocked(const char *text) noexcept {
  const std::size_t slot = (g_outputHead + g_outputCount) % kMaxOutputLines;
  std::snprintf(g_outputBuf[slot].text, kMaxLineLen - 1U + 1U, "%s", text);
  g_outputBuf[slot].text[kMaxLineLen - 1U] = '\0';

  if (g_outputCount < kMaxOutputLines) {
    ++g_outputCount;
  } else {
    g_outputHead = (g_outputHead + 1U) % kMaxOutputLines;
  }
}

// ---- built-in command handlers ----

void builtin_help(const char *const * /*args*/, int /*argCount*/,
                  void * /*userData*/) noexcept {
  CommandEntry commands[kMaxCommands] = {};
  std::size_t commandCount = 0U;

  {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (std::size_t i = 0U; i < g_commandCount; ++i) {
      if (!g_commands[i].used) {
        continue;
      }
      commands[commandCount++] = g_commands[i];
    }
  }

  console_print("Registered commands:");
  for (std::size_t i = 0U; i < commandCount; ++i) {
    char buf[kMaxLineLen] = {};
    std::snprintf(buf, sizeof(buf), "  %-20.63s  %.127s", commands[i].name,
                  commands[i].desc);
    console_print(buf);
  }
}

/// Handles builtin set.
void builtin_set(const char *const *args, int argCount,
                 void * /*userData*/) noexcept {
  if (argCount < 3) {
    console_print("Usage: set <cvar_name> <value>");
    return;
  }
  if (!cvar_set_from_string(args[1], args[2])) {
    char buf[kMaxLineLen] = {};
    std::snprintf(buf, sizeof(buf), "CVar '%s' not found or type error.",
                  args[1]);
    console_print(buf);
    return;
  }
  char buf[kMaxLineLen] = {};
  std::snprintf(buf, sizeof(buf), "set %s = %s", args[1], args[2]);
  console_print(buf);
}

/// Handles builtin get.
void builtin_get(const char *const *args, int argCount,
                 void * /*userData*/) noexcept {
  if (argCount < 2) {
    console_print("Usage: get <cvar_name>");
    return;
  }
  // Try each type
  CVarInfo info[1] = {};
  const char *name = args[1];
  // Gather all CVars and find matching type
  constexpr std::size_t kMaxInfos = 256U;
  CVarInfo infos[kMaxInfos] = {};
  const std::size_t total = cvar_get_all(infos, kMaxInfos);
  char buf[kMaxLineLen] = {};
  for (std::size_t i = 0U; i < total; ++i) {
    if (std::strcmp(infos[i].name, name) == 0) {
      switch (infos[i].type) {
      case CVarType::Bool:
        std::snprintf(buf, sizeof(buf), "%s = %s (bool)", name,
                      cvar_get_bool(name) ? "true" : "false");
        break;
      case CVarType::Int:
        std::snprintf(buf, sizeof(buf), "%s = %d (int)", name,
                      cvar_get_int(name));
        break;
      case CVarType::Float:
        std::snprintf(buf, sizeof(buf), "%s = %g (float)", name,
                      static_cast<double>(cvar_get_float(name)));
        break;
      case CVarType::String:
        std::snprintf(buf, sizeof(buf), "%s = \"%s\" (string)", name,
                      cvar_get_string(name));
        break;
      default:
        std::snprintf(buf, sizeof(buf), "%s = (unknown type)", name);
        break;
      }
      console_print(buf);
      return;
    }
  }
  std::snprintf(buf, sizeof(buf), "CVar '%s' not found.", name);
  console_print(buf);
  static_cast<void>(info);
}

// ---- tokenizer ----
// Splits `line` on whitespace into args[].  Returns arg count.
// Writes null-terminators into `lineBuf` (caller provides a mutable copy).
int tokenize(char *lineBuf, const char *const **outArgs, const char **argPtrs,
             int maxArgs) noexcept {
  int count = 0;
  char *p = lineBuf;
  while ((*p != '\0') && (count < maxArgs)) {
    // skip whitespace
    while ((*p == ' ') || (*p == '\t')) {
      ++p;
    }
    if (*p == '\0') {
      break;
    }
    argPtrs[count++] = p;
    // advance to end of token
    while ((*p != ' ') && (*p != '\t') && (*p != '\0')) {
      ++p;
    }
    if (*p != '\0') {
      *p = '\0';
      ++p;
    }
  }
  *outArgs = argPtrs;
  return count;
}

} // namespace

// ---- public API ----

bool initialize_console() noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_initialized) {
    return true;
  }
  g_commands = {};
  g_commandCount = 0U;
  g_outputBuf = {};
  g_outputHead = 0U;
  g_outputCount = 0U;
  g_initialized = true;

  // register built-ins
  static_cast<void>(register_command_unlocked(
      "help", builtin_help, nullptr, "List all registered commands"));
  static_cast<void>(register_command_unlocked(
      "set", builtin_set, nullptr, "Set a CVar: set <name> <value>"));
  static_cast<void>(register_command_unlocked(
      "get", builtin_get, nullptr, "Get a CVar value: get <name>"));
  return true;
}

/// Shuts down the owning system for console.
void shutdown_console() noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_commands = {};
  g_commandCount = 0U;
  g_outputBuf = {};
  g_outputHead = 0U;
  g_outputCount = 0U;
  g_initialized = false;
}

/// Handles console register command.
bool console_register_command(const char *name, ConsoleCommandFn fn,
                              void *userData,
                              const char *description) noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  return register_command_unlocked(name, fn, userData, description);
}

/// Handles console execute.
bool console_execute(const char *line) noexcept {
  if (line == nullptr) {
    return false;
  }

  // Copy into mutable buffer for tokenization
  char lineBuf[kMaxInputLen] = {};
  std::snprintf(lineBuf, kMaxInputLen - 1U + 1U, "%s", line);

  const char *argPtrs[kMaxArgs] = {};
  const char *const *args = nullptr;
  const int argCount = tokenize(lineBuf, &args, argPtrs, kMaxArgs);
  if (argCount == 0) {
    return false;
  }

  // Echo the command
  char echo[kMaxLineLen] = {};
  std::snprintf(echo, sizeof(echo), "> %s", line);
  console_print(echo);

  ConsoleCommandFn fn = nullptr;
  void *userData = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (std::size_t i = 0U; i < g_commandCount; ++i) {
      if (g_commands[i].used && std::strcmp(g_commands[i].name, args[0]) == 0) {
        fn = g_commands[i].fn;
        userData = g_commands[i].userData;
        break;
      }
    }
  }

  if (fn != nullptr) {
    fn(args, argCount, userData);
    return true;
  }

  char notfound[kMaxLineLen] = {};
  std::snprintf(notfound, sizeof(notfound), "Unknown command: '%s'", args[0]);
  console_print(notfound);
  return false;
}

/// Handles console print.
void console_print(const char *text) noexcept {
  if (text == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(g_mutex);
  print_unlocked(text);
}

/// Handles console output line count.
std::size_t console_output_line_count() noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_outputCount;
}

/// Handles console get output line.
bool console_get_output_line(std::size_t index, char *outBuf,
                             std::size_t bufCapacity) noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  if ((outBuf == nullptr) || (bufCapacity == 0U) || (index >= g_outputCount)) {
    return false;
  }
  const std::size_t slot = (g_outputHead + index) % kMaxOutputLines;
  std::snprintf(outBuf, bufCapacity - 1U + 1U, "%s", g_outputBuf[slot].text);
  outBuf[bufCapacity - 1U] = '\0';
  return true;
}

/// Handles console get commands.
std::size_t console_get_commands(ConsoleCommandInfo *out,
                                 std::size_t maxEntries) noexcept {
  if (out == nullptr) {
    return 0U;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  std::size_t written = 0U;
  for (std::size_t i = 0U; (i < g_commandCount) && (written < maxEntries);
       ++i) {
    if (!g_commands[i].used) {
      continue;
    }
    std::snprintf(g_commandInfoSnapshot.names[written],
                  sizeof(g_commandInfoSnapshot.names[written]), "%s",
                  g_commands[i].name);
    std::snprintf(g_commandInfoSnapshot.descriptions[written],
                  sizeof(g_commandInfoSnapshot.descriptions[written]), "%s",
                  g_commands[i].desc);
    out[written].name = g_commandInfoSnapshot.names[written];
    out[written].description = g_commandInfoSnapshot.descriptions[written];
    ++written;
  }
  return written;
}

} // namespace engine::core
