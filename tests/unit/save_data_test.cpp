// Verifies the game's save slots over explicit directories:
// - byte-exact round trips (directory creation, rewrites, an empty payload,
//   1 MiB, exactly at the project's limit) and one byte past the limit
//   refused with the previous save intact; a lowered limit still reads an
//   earlier, larger save;
// - slot names: tokens only, case ignored, slots independent;
// - every damage a slot file can carry (cut off, extended, a changed byte,
//   a broken header, a newer format, an unknown header key) read as
//   Corrupt or Unsupported, never as a shorter or different save;
// - a failed read reported as a failure, not a load;
// - listing, sorted, from headers alone, with damaged and legacy slots
//   and a capacity smaller than the slot count;
// - at most kMaxSaveSlots slots;
// - per-slot holds kept until discarded, discards moving files aside and
//   never deleting them;
// - a legacy save.json read as the default slot, retired on its first save
//   and never brought back by a discard.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include "../test_harness.h"
#include "engine/core/platform.h"
#include "engine/runtime/save_data.h"

namespace {

namespace fs = std::filesystem;
namespace rt = engine::runtime;

engine::tests::TestContext g_tests;

/// A fresh scratch directory under the OS temp dir, nested so the saves
/// folder's parents are created too.
std::string fresh_directory(const char *name) {
  char tempDir[512] = {};
  if (!engine::core::platform_get_temp_dir(tempDir, sizeof(tempDir))) {
    return {};
  }
  const fs::path root = fs::path(tempDir) / "engine_save_test" / name;
  std::error_code ec{};
  fs::remove_all(root, ec);
  return (root / "nested").string();
}

void remove_directory(const std::string &directory) {
  std::error_code ec{};
  fs::remove_all(fs::path(directory).parent_path(), ec);
}

std::string slot_file(const std::string &directory, const char *slot) {
  return (fs::path(directory) / "saves" / (std::string(slot) + ".save"))
      .string();
}

std::string read_text(const std::string &path) {
  std::string text{};
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path.c_str(), "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path.c_str(), "rb");
#endif
  if (file == nullptr) {
    return text;
  }
  char chunk[4096] = {};
  std::size_t read = 0U;
  while ((read = std::fread(chunk, 1U, sizeof(chunk), file)) > 0U) {
    text.append(chunk, read);
  }
  std::fclose(file);
  return text;
}

bool write_text(const std::string &path, const std::string &text) {
  std::error_code ec{};
  fs::create_directories(fs::path(path).parent_path(), ec);
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path.c_str(), "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path.c_str(), "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const bool written =
      text.empty() ||
      (std::fwrite(text.data(), 1U, text.size(), file) == text.size());
  return (std::fclose(file) == 0) && written;
}

/// Saves and reads back `payload`; true when the same bytes return.
bool roundtrips(const std::string &directory, const char *slot,
                const std::string &payload) {
  rt::SaveSlotPayload loaded{};
  return rt::save_game_data_to(directory.c_str(), slot, payload.data(),
                               payload.size()) &&
         (rt::read_game_data_from(directory.c_str(), slot, &loaded) ==
          rt::SaveReadResult::Ok) &&
         (loaded.length == payload.size()) &&
         (std::memcmp(loaded.data, payload.data(), payload.size()) == 0) &&
         (loaded.data[loaded.length] == '\0');
}

rt::SaveReadResult read_status(const std::string &directory, const char *slot) {
  rt::SaveSlotPayload loaded{};
  const rt::SaveReadResult result =
      rt::read_game_data_from(directory.c_str(), slot, &loaded);
  if ((result != rt::SaveReadResult::Ok) &&
      ((loaded.data != nullptr) || (loaded.length != 0U))) {
    return rt::SaveReadResult::Ok;
  }
  return result;
}

/// A payload whose bytes vary with position, so a dropped, repeated or
/// reordered block would not compare equal.
std::string patterned(std::size_t length) {
  std::string text(length, ' ');
  for (std::size_t i = 0U; i < length; ++i) {
    text[i] = static_cast<char>('!' + ((i * 7U + (i >> 10U)) % 90U));
  }
  return text;
}

void check_round_trips() {
  const std::string directory = fresh_directory("roundtrip");
  const std::string first = "{\"entries\":[{\"k\":\"best_time\",\"v\":12.5}]}";
  g_tests.check(roundtrips(directory, "default", first),
                "a save into a new nested directory reads back exactly");
  g_tests.check(roundtrips(directory, "default", "{\"entries\":[]}"),
                "a rewrite replaces the slot");
  g_tests.check(roundtrips(directory, "default", ""),
                "an empty payload reads back empty");
  const std::string text = read_text(slot_file(directory, "default"));
  g_tests.check(text.rfind("{\"format\":\"engine-save\",\"version\":1,", 0) ==
                        0U &&
                    (text.back() == '\n'),
                "the file is a one-line header and the payload");
  remove_directory(directory);
}

void check_limit() {
  const std::string directory = fresh_directory("limit");
  g_tests.check(rt::save_slot_limit() == rt::kDefaultSaveSlotLimitBytes,
                "the limit starts at the engine's default");
  g_tests.check(roundtrips(directory, "big", patterned(1024U * 1024U)),
                "a 1 MiB save round-trips");
  const std::string atLimit = patterned(rt::kDefaultSaveSlotLimitBytes);
  g_tests.check(roundtrips(directory, "big", atLimit),
                "a save exactly at the limit round-trips");
  const std::string past = patterned(rt::kDefaultSaveSlotLimitBytes + 1U);
  g_tests.check(!rt::save_game_data_to(directory.c_str(), "big", past.data(),
                                       past.size()),
                "one byte past the limit is refused");
  rt::SaveSlotPayload loaded{};
  g_tests.check(
      (rt::read_game_data_from(directory.c_str(), "big", &loaded) ==
       rt::SaveReadResult::Ok) &&
          (loaded.length == atLimit.size()) &&
          (std::memcmp(loaded.data, atLimit.data(), atLimit.size()) == 0),
      "the refused save left the previous one intact");

  g_tests.check(!rt::set_save_slot_limit(0U) &&
                    !rt::set_save_slot_limit(rt::kSaveSlotCeilingBytes + 1U) &&
                    (rt::save_slot_limit() == rt::kDefaultSaveSlotLimitBytes),
                "a limit of 0 or past the ceiling is refused");
  g_tests.check(rt::set_save_slot_limit(1024U * 1024U),
                "a 1 MiB limit is accepted");
  const std::string twoMiB = patterned(2U * 1024U * 1024U);
  g_tests.check(!rt::save_game_data_to(directory.c_str(), "other",
                                       twoMiB.data(), twoMiB.size()),
                "a lowered limit refuses a larger save");
  g_tests.check(read_status(directory, "big") == rt::SaveReadResult::Ok,
                "a lowered limit still reads a larger save written before");
  g_tests.check(rt::set_save_slot_limit(rt::kDefaultSaveSlotLimitBytes),
                "the default limit is restored");
  remove_directory(directory);
}

void check_slot_names() {
  const std::string directory = fresh_directory("names");
  const char *const refused[] = {"",     "a/b",
                                 "a\\b", "has space",
                                 "x:y",  "abcdefghijklmnopqrstuvwxyz123456"};
  for (const char *slot : refused) {
    g_tests.check(!rt::save_game_data_to(directory.c_str(), slot, "{}", 2U),
                  "a slot name that is not a token is refused");
  }
  g_tests.check(!rt::save_game_data_to(directory.c_str(), nullptr, "{}", 2U),
                "a null slot name is refused");
  g_tests.check(rt::save_slot_name_is_valid("abcdefghijklmnopqrstuvwxyz12345"),
                "31 characters is a slot name");
  g_tests.check(roundtrips(directory, "Hero_1", "{\"a\":1}"),
                "a mixed-case slot saves");
  g_tests.check(fs::exists(slot_file(directory, "hero_1")),
                "it is stored lower-case");
  rt::SaveSlotPayload loaded{};
  g_tests.check((rt::read_game_data_from(directory.c_str(), "HERO_1",
                                         &loaded) == rt::SaveReadResult::Ok) &&
                    (std::strcmp(loaded.data, "{\"a\":1}") == 0),
                "slot names ignore case");
  g_tests.check(roundtrips(directory, "slot2", "{\"b\":2}") &&
                    roundtrips(directory, "hero_1", "{\"a\":1}"),
                "slots are independent");
  g_tests.check(read_status(directory, "nothing") == rt::SaveReadResult::Absent,
                "a slot never saved is absent");
  remove_directory(directory);
}

/// Saves "{\"coins\":3}" to `slot`, then rewrites its file with `edit`
/// applied and reads it back.
template <typename Edit>
rt::SaveReadResult read_after(const std::string &directory, const char *slot,
                              Edit edit) {
  const char payload[] = "{\"coins\":3}";
  if (!rt::save_game_data_to(directory.c_str(), slot, payload,
                             sizeof(payload) - 1U)) {
    return rt::SaveReadResult::Ok;
  }
  std::string text = read_text(slot_file(directory, slot));
  edit(&text);
  if (!write_text(slot_file(directory, slot), text)) {
    return rt::SaveReadResult::Ok;
  }
  return read_status(directory, slot);
}

void check_damage() {
  const std::string directory = fresh_directory("damage");
  using R = rt::SaveReadResult;
  g_tests.check(read_after(directory, "cut",
                           [](std::string *t) { t->pop_back(); }) == R::Corrupt,
                "a save cut off by one byte is corrupt");
  g_tests.check(read_after(directory, "longer",
                           [](std::string *t) { t->push_back(' '); }) ==
                    R::Corrupt,
                "a save with a byte past its payload is corrupt");
  g_tests.check(read_after(directory, "flipped",
                           [](std::string *t) { t->back() = '4'; }) ==
                    R::Corrupt,
                "a save with a changed payload byte fails its checksum");
  g_tests.check(read_after(directory, "noheader",
                           [](std::string *t) { *t = "{\"coins\":3}"; }) ==
                    R::Corrupt,
                "a file without a header line is corrupt");
  g_tests.check(read_after(directory, "notjson",
                           [](std::string *t) { (*t)[0] = '['; }) == R::Corrupt,
                "a header that is not an object is corrupt");
  g_tests.check(read_after(directory, "format",
                           [](std::string *t) {
                             t->replace(t->find("engine-save"), 11U,
                                        "other-save!");
                           }) == R::Corrupt,
                "a header of another format is corrupt");
  g_tests.check(read_after(directory, "newer",
                           [](std::string *t) {
                             t->replace(t->find("\"version\":1"), 11U,
                                        "\"version\":2");
                           }) == R::Unsupported,
                "a header from a newer format is unsupported");
  g_tests.check(read_after(directory, "extra",
                           [](std::string *t) { t->insert(1U, "\"x\":0,"); }) ==
                    R::Corrupt,
                "a header with a key it does not define is corrupt");
  g_tests.check(read_after(directory, "checksum",
                           [](std::string *t) {
                             const std::size_t at = t->find("checksum\":\"");
                             (*t)[at + 11U] = 'G';
                           }) == R::Corrupt,
                "a checksum that is not hex is corrupt");

  // A directory planted where the slot file belongs: a read fault, never
  // a successful read of nothing.
  std::error_code ec{};
  fs::create_directories(fs::path(slot_file(directory, "planted")), ec);
  rt::SaveSlotPayload loaded{};
  g_tests.check((rt::read_game_data_from(directory.c_str(), "planted",
                                         &loaded) != rt::SaveReadResult::Ok) &&
                    (loaded.data == nullptr) && (loaded.length == 0U),
                "a failed read is a failure with nothing loaded");
  remove_directory(directory);
}

void check_listing() {
  const std::string directory = fresh_directory("listing");
  rt::SaveSlotInfo slots[8] = {};
  g_tests.check(rt::list_game_saves_in(directory.c_str(), slots, 8U) == 0U,
                "a directory with no saves lists none");
  const char *const names[] = {"zeta", "alpha", "mid"};
  for (const char *name : names) {
    g_tests.check(rt::save_game_data_to(directory.c_str(), name, "{}", 2U),
                  "a listing fixture saves");
  }
  g_tests.check(write_text(slot_file(directory, "broken"), "not a save"),
                "a damaged slot is planted");
  g_tests.check(
      write_text(slot_file(directory, "Upper"), "x") &&
          write_text((fs::path(directory) / "saves" / "notes.txt").string(),
                     "x"),
      "files no save writes are planted");
  const std::size_t total =
      rt::list_game_saves_in(directory.c_str(), slots, 8U);
  g_tests.check(total == 4U, "every slot file is listed and nothing else");
  g_tests.check((std::strcmp(slots[0].slot, "alpha") == 0) &&
                    (std::strcmp(slots[1].slot, "broken") == 0) &&
                    (std::strcmp(slots[2].slot, "mid") == 0) &&
                    (std::strcmp(slots[3].slot, "zeta") == 0),
                "slots list sorted by name");
  g_tests.check((slots[0].status == rt::SaveReadResult::Ok) &&
                    (slots[0].payloadBytes == 2U) && (slots[0].savedAt > 0) &&
                    !slots[0].legacy,
                "a slot lists its size and save time");
  g_tests.check(slots[1].status == rt::SaveReadResult::Corrupt,
                "a damaged slot lists as corrupt");
  rt::SaveSlotInfo two[2] = {};
  g_tests.check((rt::list_game_saves_in(directory.c_str(), two, 2U) == 4U) &&
                    (std::strcmp(two[0].slot, "alpha") == 0) &&
                    (std::strcmp(two[1].slot, "broken") == 0),
                "a short listing keeps the lowest names and counts them all");
  remove_directory(directory);
}

void check_slot_count() {
  const std::string directory = fresh_directory("count");
  bool all = true;
  char name[16] = {};
  for (std::size_t i = 0U; i < rt::kMaxSaveSlots; ++i) {
    std::snprintf(name, sizeof(name), "s%zu", i);
    all = rt::save_game_data_to(directory.c_str(), name, "{}", 2U) && all;
  }
  g_tests.check(all, "kMaxSaveSlots slots save");
  g_tests.check(!rt::save_game_data_to(directory.c_str(), "one_more", "{}", 2U),
                "a new slot past kMaxSaveSlots is refused");
  g_tests.check(rt::save_game_data_to(directory.c_str(), "s0", "{\"x\":1}", 7U),
                "an existing slot still saves at the limit");
  g_tests.check(
      rt::discard_game_save_in(directory.c_str(), "s1") &&
          rt::save_game_data_to(directory.c_str(), "one_more", "{}", 2U),
      "discarding a slot makes room for a new one");
  remove_directory(directory);
}

void check_holds() {
  const std::string directory = fresh_directory("holds");
  const char *dir = directory.c_str();
  const std::string file = slot_file(directory, "kept");
  g_tests.check(write_text(file, "damaged"), "a damaged slot is planted");
  g_tests.check(read_status(directory, "kept") == rt::SaveReadResult::Corrupt,
                "it reads as corrupt");
  rt::hold_game_save_in(dir, "kept");
  g_tests.check(rt::game_save_held_in(dir, "KEPT") &&
                    !rt::save_game_data_to(dir, "kept", "{}", 2U) &&
                    (read_text(file) == "damaged"),
                "a held slot refuses saves and keeps its file");
  g_tests.check(rt::save_game_data_to(dir, "other", "{}", 2U),
                "other slots save while one is held");
  g_tests.check(rt::discard_game_save_in(dir, "kept") &&
                    !rt::game_save_held_in(dir, "kept") &&
                    (read_text(file + ".discarded-1") == "damaged") &&
                    !fs::exists(file),
                "discarding moves the file aside intact and lifts the hold");
  g_tests.check(roundtrips(directory, "kept", "{\"fresh\":1}"),
                "the next save starts a new file");
  rt::hold_game_save_in(dir, "kept");
  g_tests.check(rt::discard_game_save_in(dir, "kept") &&
                    fs::exists(file + ".discarded-2") &&
                    (read_text(file + ".discarded-1") == "damaged"),
                "a second discard takes the next free name");
  rt::hold_game_save_in(dir, "kept");
  g_tests.check(rt::discard_game_save_in(dir, "kept") &&
                    !rt::game_save_held_in(dir, "kept"),
                "discarding a slot with no file lifts the hold");
  remove_directory(directory);
}

void check_legacy() {
  const std::string directory = fresh_directory("legacy");
  const char *dir = directory.c_str();
  const std::string legacy = (fs::path(directory) / "save.json").string();
  const std::string old = "{\"entries\":[{\"k\":\"coins\",\"v\":7}]}";
  g_tests.check(write_text(legacy, old), "a legacy save.json is planted");
  rt::SaveSlotPayload loaded{};
  g_tests.check((rt::read_game_data_from(dir, "default", &loaded) ==
                 rt::SaveReadResult::Ok) &&
                    (std::string(loaded.data, loaded.length) == old),
                "a legacy save reads as the default slot");
  g_tests.check(read_status(directory, "other") == rt::SaveReadResult::Absent,
                "a legacy save is the default slot only");
  rt::SaveSlotInfo slots[2] = {};
  g_tests.check((rt::list_game_saves_in(dir, slots, 2U) == 1U) &&
                    (std::strcmp(slots[0].slot, "default") == 0) &&
                    slots[0].legacy && (slots[0].payloadBytes == old.size()),
                "a legacy save lists as the default slot");
  g_tests.check(write_text(legacy, ""), "an empty legacy save is planted");
  g_tests.check((rt::read_game_data_from(dir, "default", &loaded) ==
                 rt::SaveReadResult::Ok) &&
                    (loaded.length == 0U),
                "an empty legacy save reads as an empty payload");
  g_tests.check(write_text(legacy, old), "the legacy save is restored");

  rt::hold_game_save_in(dir, "default");
  g_tests.check(!rt::save_game_data_to(dir, "default", "{}", 2U) &&
                    (read_text(legacy) == old),
                "a held default slot keeps the legacy save");
  rt::discard_game_save_in(dir, "default");
  g_tests.check(!fs::exists(legacy) &&
                    (read_text(legacy + ".discarded-1") == old),
                "discarding the default slot moves the legacy save aside");

  g_tests.check(write_text(legacy, old), "a legacy save is planted again");
  g_tests.check(roundtrips(directory, "default", "{\"new\":1}"),
                "the default slot saves over a legacy save");
  g_tests.check(!fs::exists(legacy) &&
                    (read_text(legacy + ".migrated-1") == old),
                "its first save moves the legacy save aside, intact");
  g_tests.check(
      rt::discard_game_save_in(dir, "default") &&
          (read_status(directory, "default") == rt::SaveReadResult::Absent),
      "a discard never brings the legacy save back");
  remove_directory(directory);
}

} // namespace

int main() {
  check_round_trips();
  check_limit();
  check_slot_names();
  check_damage();
  check_listing();
  check_slot_count();
  check_holds();
  check_legacy();
  return g_tests.finish("save slot tests");
}
