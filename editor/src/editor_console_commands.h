// Declares the Log panel's command line helpers: the history of entered
// commands, stepped through with Up and Down, and Tab completion of a
// command name, or of a cvar name after `set ` or `get `, over what the
// console and the cvar table hold. Both are fixed-size and allocate
// nothing, so the panel can drive them every frame.

#pragma once

#include <array>
#include <cstddef>

namespace engine::editor {

/// Longest command line the Log panel accepts, terminator included.
inline constexpr std::size_t kConsoleCommandCapacity = 256U;
/// Commands the history keeps; the oldest goes first when it is full.
inline constexpr std::size_t kConsoleHistoryCapacity = 32U;

/// The commands entered, oldest first, and a cursor for stepping through
/// them from the newest back, as a shell's history does.
class ConsoleHistory final {
public:
  /// Records `line` as the newest entry and leaves browsing. An empty line,
  /// one equal to the newest entry, or one longer than
  /// kConsoleCommandCapacity allows is not recorded.
  void push(const char *line) noexcept;
  /// Entries held.
  std::size_t size() const noexcept { return m_count; }
  /// Entry `index`, 0 the oldest; nullptr past the end.
  const char *at(std::size_t index) const noexcept;
  /// Steps to the next older entry and returns it, staying on the oldest;
  /// nullptr when the history is empty.
  const char *older() noexcept;
  /// Steps to the next newer entry and returns it; stepping past the newest
  /// leaves browsing and returns "", the fresh empty line. nullptr when not
  /// browsing.
  const char *newer() noexcept;
  /// Leaves browsing: the next older() returns the newest entry.
  void reset_cursor() noexcept { m_cursor = m_count; }

private:
  std::array<std::array<char, kConsoleCommandCapacity>, kConsoleHistoryCapacity>
      m_lines{};
  std::size_t m_oldest = 0U;
  std::size_t m_count = 0U;
  /// The entry shown, 0 the oldest; m_count when not browsing.
  std::size_t m_cursor = 0U;
};

/// Completes the last word of `line`: the command name, or the cvar name
/// after `set ` or `get `, against the registered console commands and
/// cvars. One match completes whole, followed by a space; several complete
/// to the longest prefix they share. Writes the completed line to `out`
/// and, when there are several matches, their names separated by two
/// spaces to `candidates` (display text, cut to fit). Returns the number of
/// matches; with none, or when the completed line would not fit `capacity`,
/// `out` holds `line` unchanged.
std::size_t complete_console_line(const char *line, char *out,
                                  std::size_t capacity, char *candidates,
                                  std::size_t candidatesCapacity) noexcept;

} // namespace engine::editor
