// Implements the Log panel's command history and Tab completion over the
// console's commands and the cvar table.

#include "editor_console_commands.h"

#include <cstdio>
#include <cstring>

#include "engine/core/console.h"
#include "engine/core/cvar.h"

namespace engine::editor {

namespace {

constexpr std::size_t kMaxCommands = 128U;
constexpr std::size_t kMaxCvars = 256U;

/// Length of the prefix `a` and `b` share.
std::size_t shared_prefix(const char *a, const char *b) noexcept {
  std::size_t length = 0U;
  while ((a[length] != '\0') && (a[length] == b[length])) {
    ++length;
  }
  return length;
}

/// What the completion has found so far: how many names match, the first
/// match, and how much every match shares with it.
struct Matches final {
  std::size_t count = 0U;
  const char *first = nullptr;
  std::size_t sharedLength = 0U;
  char *candidates = nullptr;
  std::size_t candidatesCapacity = 0U;
  std::size_t candidatesLength = 0U;
};

/// Counts `name` when it starts with `word`, and lists it.
void consider(Matches *matches, const char *name, const char *word,
              std::size_t wordLength) noexcept {
  if ((name == nullptr) || (std::strncmp(name, word, wordLength) != 0)) {
    return;
  }
  if (matches->count == 0U) {
    matches->first = name;
    matches->sharedLength = std::strlen(name);
  } else {
    const std::size_t shared = shared_prefix(matches->first, name);
    if (shared < matches->sharedLength) {
      matches->sharedLength = shared;
    }
  }
  ++matches->count;

  // The list is display text: a name that does not fit is left off.
  if ((matches->candidates == nullptr) || (matches->candidatesCapacity == 0U)) {
    return;
  }
  const char *separator = (matches->candidatesLength > 0U) ? "  " : "";
  const std::size_t needed = std::strlen(separator) + std::strlen(name);
  if (matches->candidatesLength + needed < matches->candidatesCapacity) {
    std::memcpy(matches->candidates + matches->candidatesLength, separator,
                std::strlen(separator));
    matches->candidatesLength += std::strlen(separator);
    std::memcpy(matches->candidates + matches->candidatesLength, name,
                std::strlen(name));
    matches->candidatesLength += std::strlen(name);
    matches->candidates[matches->candidatesLength] = '\0';
  }
}

/// Copies `text` into `out` whole; false, `out` untouched, when it does not
/// fit.
bool copy_whole(const char *text, char *out, std::size_t capacity) noexcept {
  const std::size_t length = std::strlen(text);
  if (length >= capacity) {
    return false;
  }
  std::memcpy(out, text, length + 1U);
  return true;
}

} // namespace

void ConsoleHistory::push(const char *line) noexcept {
  reset_cursor();
  if ((line == nullptr) || (line[0] == '\0') ||
      (std::strlen(line) >= kConsoleCommandCapacity)) {
    return;
  }
  if ((m_count > 0U) && (std::strcmp(at(m_count - 1U), line) == 0)) {
    return;
  }
  std::size_t slot = 0U;
  if (m_count < kConsoleHistoryCapacity) {
    slot = (m_oldest + m_count) % kConsoleHistoryCapacity;
    ++m_count;
  } else {
    slot = m_oldest;
    m_oldest = (m_oldest + 1U) % kConsoleHistoryCapacity;
  }
  std::memcpy(m_lines[slot].data(), line, std::strlen(line) + 1U);
  reset_cursor();
}

const char *ConsoleHistory::at(std::size_t index) const noexcept {
  if (index >= m_count) {
    return nullptr;
  }
  return m_lines[(m_oldest + index) % kConsoleHistoryCapacity].data();
}

const char *ConsoleHistory::older() noexcept {
  if (m_count == 0U) {
    return nullptr;
  }
  if (m_cursor > 0U) {
    --m_cursor;
  }
  return at(m_cursor);
}

const char *ConsoleHistory::newer() noexcept {
  if (m_cursor >= m_count) {
    return nullptr;
  }
  ++m_cursor;
  return (m_cursor == m_count) ? "" : at(m_cursor);
}

bool run_console_line(const char *line, bool playing) noexcept {
  if ((line == nullptr) || (line[0] == '\0')) {
    return false;
  }
  if (!playing && core::console_line_changes_world(line)) {
    char echo[kConsoleCommandCapacity + 4U] = {};
    std::snprintf(echo, sizeof(echo), "> %s", line);
    core::console_print(echo);
    core::console_print("Refused: this command changes the running game and "
                        "works only in Play. In Edit mode, add objects with "
                        "the Create menu or by dragging a prefab from Assets, "
                        "so they can be undone and saved.");
    return false;
  }
  return core::console_execute(line);
}

std::size_t complete_console_line(const char *line, char *out,
                                  std::size_t capacity, char *candidates,
                                  std::size_t candidatesCapacity) noexcept {
  if ((line == nullptr) || (out == nullptr) || (capacity == 0U)) {
    return 0U;
  }
  if ((candidates != nullptr) && (candidatesCapacity > 0U)) {
    candidates[0] = '\0';
  }
  if (!copy_whole(line, out, capacity)) {
    out[0] = '\0';
    return 0U;
  }

  // The word being completed: the command name while there is no space,
  // the cvar name after "set " or "get ", nothing anywhere else.
  const char *space = std::strchr(line, ' ');
  bool cvarWord = false;
  const char *word = line;
  if (space != nullptr) {
    const std::size_t commandLength = static_cast<std::size_t>(space - line);
    const bool takesCvar =
        (commandLength == 3U) && ((std::strncmp(line, "set", 3U) == 0) ||
                                  (std::strncmp(line, "get", 3U) == 0));
    if (!takesCvar || (std::strchr(space + 1, ' ') != nullptr)) {
      return 0U;
    }
    cvarWord = true;
    word = space + 1;
  }
  const std::size_t wordLength = std::strlen(word);

  Matches matches{};
  matches.candidates = candidates;
  matches.candidatesCapacity = candidatesCapacity;
  if (cvarWord) {
    std::array<core::CVarInfo, kMaxCvars> cvars =
        std::array<core::CVarInfo, kMaxCvars>();
    const std::size_t count = core::cvar_get_all(cvars.data(), cvars.size());
    for (std::size_t i = 0U; i < count; ++i) {
      consider(&matches, cvars[i].name, word, wordLength);
    }
  } else {
    std::array<core::ConsoleCommandInfo, kMaxCommands> commands =
        std::array<core::ConsoleCommandInfo, kMaxCommands>();
    const std::size_t count =
        core::console_get_commands(commands.data(), commands.size());
    for (std::size_t i = 0U; i < count; ++i) {
      consider(&matches, commands[i].name, word, wordLength);
    }
  }
  if (matches.count == 0U) {
    return 0U;
  }

  // The completed line: what precedes the word, then the match (whole and
  // a space for one, the shared prefix for several).
  const std::size_t leadLength = static_cast<std::size_t>(word - line);
  const std::size_t matchLength = matches.sharedLength;
  const bool single = (matches.count == 1U);
  const std::size_t total = leadLength + matchLength + (single ? 1U : 0U);
  if (total >= capacity) {
    // `line` fit above, so this restores it whole.
    static_cast<void>(copy_whole(line, out, capacity));
    return 0U;
  }
  std::memcpy(out, line, leadLength);
  std::memcpy(out + leadLength, matches.first, matchLength);
  if (single) {
    out[leadLength + matchLength] = ' ';
  }
  out[total] = '\0';
  return matches.count;
}

} // namespace engine::editor
