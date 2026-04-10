#pragma once

#include <cstddef>

// Developer console command system.
// Commands are registered by name with a callback.  `console_execute` parses
// an input line, looks up the command, and dispatches it.
// Output from commands is stored in a fixed-size ring buffer and readable via
// `console_get_output_line`.  Safe to call from any thread after
// initialize_console().

namespace engine::core {

// Signature: receives a null-terminated argv-style array of `argCount` tokens.
// args[0] is always the command name.
// NOLINTNEXTLINE(modernize-use-using)
typedef void (*ConsoleCommandFn)(const char *const *args, int argCount,
                                 void *userData) noexcept;

struct ConsoleCommandInfo final {
  const char *name = nullptr;
  const char *description = nullptr;
};

bool initialize_console() noexcept;
void shutdown_console() noexcept;

// Register a command.  Returns false on duplicate name or capacity exceeded.
bool console_register_command(const char *name, ConsoleCommandFn fn,
                              void *userData, const char *description) noexcept;

// Parse `line`, find the command, and invoke its callback.
// Returns false if the command is not found.
bool console_execute(const char *line) noexcept;

// Append a string to the output ring buffer (used internally and by C++ code).
void console_print(const char *text) noexcept;

// Returns the number of lines currently in the output buffer.
std::size_t console_output_line_count() noexcept;

// Copy line `index` (0 = oldest) into `outBuf` (null-terminated).
// Returns false if index is out of range or outBuf is null.
bool console_get_output_line(std::size_t index, char *outBuf,
                             std::size_t bufCapacity) noexcept;

// Enumerate registered commands.  Returns number of entries written.
std::size_t console_get_commands(ConsoleCommandInfo *out,
                                 std::size_t maxEntries) noexcept;

} // namespace engine::core
