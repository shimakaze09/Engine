// Declares console types and APIs for the Engine core engine.

#pragma once

#include <cstddef>

// Developer console command system.
// Commands are registered by name with a callback.  `console_execute` parses
// an input line, looks up the command, and dispatches it.
// Output from commands, the echoed command line included, is logged at Info
// on the "console" channel, so it shows wherever the log does (the editor's
// Log panel), and is also kept in a fixed-size ring buffer readable via
// `console_get_output_line`.  Safe to call from any thread after
// initialize_console().

namespace engine::core {

// Signature: receives a null-terminated argv-style array of `argCount` tokens.
// args[0] is always the command name.
// NOLINTNEXTLINE(modernize-use-using)
typedef void (*ConsoleCommandFn)(const char *const *args, int argCount,
                                 void *userData) noexcept;

/// Registered command: name, help text, and handler.
struct ConsoleCommandInfo final {
  const char *name = nullptr;
  const char *description = nullptr;
};

/// Initializes the owning system for console.
bool initialize_console() noexcept;
/// Shuts down the owning system for console.
void shutdown_console() noexcept;

// Register a command.  Returns false on duplicate name or capacity exceeded.
bool console_register_command(const char *name, ConsoleCommandFn fn,
                              void *userData, const char *description) noexcept;

// Register a command that changes the running World (spawning, say), as
// console_register_command does. A host that also edits an authored scene
// -- the editor -- runs such a command only in a play session, where the
// change is thrown away at Stop, since it bypasses the host's undo and
// unsaved-change tracking.
bool console_register_world_command(const char *name, ConsoleCommandFn fn,
                                    void *userData,
                                    const char *description) noexcept;

// True when the first word of `line` names a command registered with
// console_register_world_command.
bool console_line_changes_world(const char *line) noexcept;

// Parse `line`, find the command, and invoke its callback.
// Returns false if the command is not found.
bool console_execute(const char *line) noexcept;

// Append a string to the output ring buffer and log it on the "console"
// channel (used internally and by C++ code).
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
