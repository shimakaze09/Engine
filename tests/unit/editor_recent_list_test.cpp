// Verifies the editor's recently-used list: it starts empty on a fresh
// profile, keeps the most recent entry first, moves a re-added entry to
// the front rather than repeating it, drops the oldest past capacity (0,
// 1, exactly capacity and one past), ignores an empty or overlong path,
// removes an entry, persists across a session (a forgotten cache reads the
// stored file back), drops stored entries that no longer exist, and never
// overwrites a stored file this session could not read: a file too large,
// unreadable, or not a list stays byte for byte as it was. A stored key
// this build does not read is named in the log.

#include "../test_harness.h"
#include "editor_recent_list.h"
#include "engine/core/logging.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace {

namespace fs = std::filesystem;
using engine::editor::RecentList;

constexpr const char *kFile = "editor_recent_test.json";
constexpr const char *kKey = "items";

/// A list of the given capacity that offers only entries that exist.
RecentList make_list(std::size_t capacity, bool prune) {
  RecentList list{};
  list.fileName = kFile;
  list.arrayKey = kKey;
  list.capacity = capacity;
  list.stillExists = prune ? &engine::editor::recent_entry_exists : nullptr;
  return list;
}

std::string read_bytes(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

bool write_bytes(const fs::path &path, const std::string &bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(out);
}

int g_unreadKeyWarnings = 0;

/// Counts the Warnings that name the stored file's unread `pinned` key.
void note_unread_key(engine::core::LogLevel level, const char * /*channel*/,
                     const char *message, void * /*userData*/) noexcept {
  if ((level == engine::core::LogLevel::Warning) && (message != nullptr) &&
      (std::strstr(message, "'pinned' is not read by this build") != nullptr)) {
    ++g_unreadKeyWarnings;
  }
}

/// The entry at `index` equals `text`.
bool is(RecentList *list, std::size_t index, const std::string &text) {
  return text == engine::editor::recent_list_at(list, index);
}

} // namespace

int main() {
  engine::tests::TestContext t;
  std::error_code ec{};
  const fs::path scratch = fs::temp_directory_path(ec) / "engine_recent_list";
  fs::remove_all(scratch, ec);
  fs::create_directories(scratch / "real", ec);
  if (ec) {
    return 2;
  }
  const std::string dir = (scratch / "store").string();
  engine::editor::recent_lists_set_directory_override_for_tests(dir.c_str());
  const fs::path stored = fs::path(dir) / kFile;

  // Fresh profile: empty, and the first add creates the file.
  RecentList list = make_list(3U, false);
  t.check(engine::editor::recent_list_count(&list) == 0U,
          "a fresh list is empty");
  t.check(engine::editor::recent_list_at(&list, 0U)[0] == '\0',
          "an entry past the end is empty");
  engine::editor::recent_list_add(&list, "a");
  t.check(engine::editor::recent_list_count(&list) == 1U && is(&list, 0U, "a"),
          "one entry");
  t.check(fs::is_regular_file(stored, ec), "the first add writes the file");

  // Order, dedupe, capacity.
  engine::editor::recent_list_add(&list, "b");
  engine::editor::recent_list_add(&list, "c");
  t.check(engine::editor::recent_list_count(&list) == 3U &&
              is(&list, 0U, "c") && is(&list, 1U, "b") && is(&list, 2U, "a"),
          "exactly at capacity, most recent first");
  engine::editor::recent_list_add(&list, "b");
  t.check(engine::editor::recent_list_count(&list) == 3U &&
              is(&list, 0U, "b") && is(&list, 1U, "c") && is(&list, 2U, "a"),
          "a repeated entry moves to the front and is not duplicated");
  engine::editor::recent_list_add(&list, "d");
  t.check(engine::editor::recent_list_count(&list) == 3U &&
              is(&list, 0U, "d") && is(&list, 1U, "b") && is(&list, 2U, "c"),
          "one past capacity drops the oldest");
  engine::editor::recent_list_add(&list, "");
  engine::editor::recent_list_add(&list, nullptr);
  const std::string tooLong(engine::editor::kMaxRecentPathLength, 'x');
  engine::editor::recent_list_add(&list, tooLong.c_str());
  t.check(engine::editor::recent_list_count(&list) == 3U && is(&list, 0U, "d"),
          "an empty, null or overlong path is ignored");
  RecentList none = make_list(0U, false);
  engine::editor::recent_list_add(&none, "z");
  t.check(engine::editor::recent_list_count(&none) == 0U,
          "a list of capacity zero keeps nothing");
  RecentList one = make_list(1U, false);
  engine::editor::recent_list_add(&one, "p");
  engine::editor::recent_list_add(&one, "q");
  t.check(engine::editor::recent_list_count(&one) == 1U && is(&one, 0U, "q"),
          "a list of capacity one keeps the latest");
  engine::editor::recent_list_forget(&one);
  fs::remove(fs::path(dir) / kFile, ec);
  fs::remove(stored, ec);

  // Remove.
  RecentList second = make_list(3U, false);
  engine::editor::recent_list_add(&second, "x");
  engine::editor::recent_list_add(&second, "y");
  engine::editor::recent_list_remove(&second, "x");
  engine::editor::recent_list_remove(&second, "not there");
  t.check(engine::editor::recent_list_count(&second) == 1U &&
              is(&second, 0U, "y"),
          "an entry is removed and a missing one is ignored");

  // Persistence, through the production reload path.
  RecentList reloaded = make_list(3U, false);
  t.check(engine::editor::recent_list_count(&reloaded) == 1U &&
              is(&reloaded, 0U, "y"),
          "a fresh session reads the stored list back");

  // Entries that no longer exist are dropped on load and the file updated.
  const std::string keep = (scratch / "real" / "keep.txt").string();
  const std::string gone = (scratch / "real" / "gone.txt").string();
  t.check(write_bytes(keep, "k"), "a file to keep");
  RecentList pruning = make_list(4U, true);
  engine::editor::recent_list_add(&pruning, gone.c_str());
  engine::editor::recent_list_add(&pruning, keep.c_str());
  RecentList afterRestart = make_list(4U, true);
  t.check(engine::editor::recent_list_count(&afterRestart) == 1U &&
              is(&afterRestart, 0U, keep),
          "a stored entry that no longer exists is dropped on load");
  t.check(read_bytes(stored).find("gone.txt") == std::string::npos,
          "and is gone from the stored file too");

  // A key this build does not read is named, since the next save drops it.
  const bool sinkRegistered =
      engine::core::initialize_logging() &&
      engine::core::log_register_sink(&note_unread_key, nullptr);
  t.check(write_bytes(stored, "{\"items\":[\"w\"],\"pinned\":[\"w\"]}"),
          "a stored list with a key this build does not read");
  RecentList newer = make_list(3U, false);
  t.check(sinkRegistered && engine::editor::recent_list_count(&newer) == 1U &&
              is(&newer, 0U, "w") && g_unreadKeyWarnings == 1,
          "it loads, and the unread key is named once");
  engine::core::log_unregister_sink(&note_unread_key, nullptr);
  engine::core::shutdown_logging();

  // A stored file the session cannot use is never overwritten.
  const std::string notAList = "{\"other\":[\"n\"]}";
  t.check(write_bytes(stored, notAList), "a stored file that is not a list");
  RecentList refused = make_list(3U, false);
  t.check(engine::editor::recent_list_count(&refused) == 0U,
          "it loads as empty");
  engine::editor::recent_list_add(&refused, "m");
  t.check(engine::editor::recent_list_count(&refused) == 1U &&
              read_bytes(stored) == notAList,
          "and a later add works in memory but leaves the file as it was");

  const std::string huge(engine::editor::kMaxStoredRecentListBytes + 100U, ' ');
  t.check(write_bytes(stored, huge), "a stored file larger than any list");
  RecentList tooLarge = make_list(3U, false);
  engine::editor::recent_list_add(&tooLarge, "m");
  t.check(read_bytes(stored) == huge, "it is left untouched");

  fs::remove(stored, ec);
  fs::create_directories(stored, ec);
  RecentList unreadable = make_list(3U, false);
  engine::editor::recent_list_add(&unreadable, "m");
  t.check(fs::is_directory(stored, ec) && !ec &&
              engine::editor::recent_list_count(&unreadable) == 1U,
          "a stored path that cannot be read is left alone");
  fs::remove_all(stored, ec);

  // Forgetting reads the stored file again, and a repaired file recovers.
  engine::editor::recent_list_forget(&unreadable);
  engine::editor::recent_list_add(&unreadable, "m");
  RecentList recovered = make_list(3U, false);
  t.check(engine::editor::recent_list_count(&recovered) == 1U &&
              is(&recovered, 0U, "m"),
          "once the file is readable the list persists again");

  engine::editor::recent_lists_set_directory_override_for_tests("");
  fs::remove_all(scratch, ec);
  return t.finish("editor_recent_list");
}
