// Implements the game's save slots over the project's data directory: the
// slot file's header (format version, save time, payload length and
// FNV-1a 64 checksum) and payload, staged atomic writes, per-slot holds,
// header-only listing, and the reading and retiring of a legacy save.json,
// with explicit-directory variants for tests.

#include "engine/runtime/save_data.h"

#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <new>
#include <string>
#include <system_error>

#include "engine/core/atomic_file.h"
#include "engine/core/file_read.h"
#include "engine/core/hash.h"
#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/core/project_data.h"
#include "engine/core/string_util.h"

namespace engine::runtime {

namespace {

constexpr const char *kLogChannel = "save";
constexpr const char *kLegacyFileName = "save.json";
constexpr const char *kSlotDirectory = "saves";
constexpr const char *kSlotExtension = ".save";
constexpr const char *kSlotFormat = "engine-save";
/// The header line's capacity, newline included. The writer's longest
/// header is about 130 bytes; anything without a newline by here is not a
/// slot file.
constexpr std::size_t kMaxHeaderBytes = 256U;
constexpr std::size_t kPathCapacity = 1100U;

std::size_t g_slotLimit = kDefaultSaveSlotLimitBytes;

/// Writes `slot` lower-cased into `out`; false when it is not a slot name.
bool lower_slot(const char *slot, char (&out)[kSaveSlotNameCapacity]) noexcept {
  if (!save_slot_name_is_valid(slot)) {
    return false;
  }
  std::size_t i = 0U;
  for (; slot[i] != '\0'; ++i) {
    out[i] = core::ascii_lower(slot[i]);
  }
  out[i] = '\0';
  return true;
}

/// Logs a refused slot name.
void log_invalid_slot(const char *slot) noexcept {
  char message[200] = {};
  std::snprintf(message, sizeof(message),
                "save slot name '%.64s' is not a name token (letters, "
                "digits, '_', '-' and '.', 1 to 31 characters)",
                (slot != nullptr) ? slot : "(null)");
  core::log_message(core::LogLevel::Error, kLogChannel, message);
}

bool format_path(char *out, std::size_t capacity, const char *format,
                 const char *directory, const char *name) noexcept {
  const int written = std::snprintf(out, capacity, format, directory, name);
  return (written > 0) && (static_cast<std::size_t>(written) < capacity);
}

/// "<directory>/saves/<slot>.save"; false when the path does not fit.
bool slot_path(const char *directory, const char *lowerSlot, char *out,
               std::size_t capacity) noexcept {
  char format[32] = {};
  std::snprintf(format, sizeof(format), "%%s/%s/%%s%s", kSlotDirectory,
                kSlotExtension);
  return format_path(out, capacity, format, directory, lowerSlot);
}

/// "<directory>/saves"; false when the path does not fit.
bool slot_directory(const char *directory, char *out,
                    std::size_t capacity) noexcept {
  return format_path(out, capacity, "%s/%s", directory, kSlotDirectory);
}

/// "<directory>/save.json"; false when the path does not fit.
bool legacy_path(const char *directory, char *out,
                 std::size_t capacity) noexcept {
  return format_path(out, capacity, "%s/%s", directory, kLegacyFileName);
}

bool path_exists(const char *path) noexcept {
  std::error_code ec{};
  return std::filesystem::exists(std::filesystem::path(path), ec) && !ec;
}

std::uint64_t payload_checksum(const char *payload,
                               std::size_t length) noexcept {
  std::uint64_t hash = core::kFnv1a64Offset;
  for (std::size_t i = 0U; i < length; ++i) {
    hash = core::fnv1a_64_append(hash, static_cast<std::uint8_t>(payload[i]));
  }
  return hash;
}

/// Says once per process that a save.json from before saves were scoped
/// per project sits in the shared directory. Nothing in it names the
/// project that wrote it, so it is neither read nor replaced: the player
/// moves it into the project directory the log names if it is theirs.
void note_unattributed_legacy_save() noexcept {
  static bool noted = false;
  if (noted) {
    return;
  }
  char sharedDir[1024] = {};
  char sharedLegacy[kPathCapacity] = {};
  if (!core::platform_get_save_dir(sharedDir, sizeof(sharedDir)) ||
      !legacy_path(sharedDir, sharedLegacy, sizeof(sharedLegacy))) {
    return;
  }
  char probe[1] = {};
  std::size_t size = 0U;
  if (core::read_file_prefix(sharedLegacy, probe, sizeof(probe), &size) ==
      core::FileReadResult::Absent) {
    return;
  }
  noted = true;
  char message[1300] = {};
  std::snprintf(message, sizeof(message),
                "%.1100s predates per-project saves and names no project; "
                "it is left untouched and not loaded",
                sharedLegacy);
  core::log_message(core::LogLevel::Info, kLogChannel, message);
}

/// The held slots, by file path. A load that cannot use a slot holds it;
/// when more slots are held than fit, every save is refused instead, since
/// an unrecorded hold would let a save replace an unread file.
constexpr std::size_t kMaxHeldSlots = 32U;
std::array<std::array<char, kPathCapacity>, kMaxHeldSlots> g_held{};
bool g_heldOverflow = false;

int held_index(const char *path) noexcept {
  for (std::size_t i = 0U; i < kMaxHeldSlots; ++i) {
    if ((g_held[i][0] != '\0') && (std::strcmp(g_held[i].data(), path) == 0)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

/// Moves `path` aside to the first free "<path>.<suffix>-<n>"; the target
/// in *target. False when every name is taken or the move fails.
bool move_aside(const char *path, const char *suffix, char *target,
                std::size_t capacity) noexcept {
  for (int n = 1; n <= 999; ++n) {
    const int written =
        std::snprintf(target, capacity, "%s.%s-%d", path, suffix, n);
    if ((written <= 0) || (static_cast<std::size_t>(written) >= capacity)) {
      return false;
    }
    std::error_code probe{};
    if (std::filesystem::exists(std::filesystem::path(target), probe) ||
        probe) {
      continue;
    }
    std::error_code ec{};
    std::filesystem::rename(std::filesystem::path(path),
                            std::filesystem::path(target), ec);
    return !ec;
  }
  return false;
}

/// What a slot header holds.
struct SlotHeader final {
  std::int64_t savedAt = 0;
  std::uint64_t payloadBytes = 0U;
  std::uint64_t checksum = 0U;
};

/// Reads a 16-digit lower-case hex checksum.
bool parse_checksum(const char *text, std::size_t length,
                    std::uint64_t *out) noexcept {
  if (length != 16U) {
    return false;
  }
  std::uint64_t value = 0U;
  for (std::size_t i = 0U; i < length; ++i) {
    const char c = text[i];
    std::uint64_t digit = 0U;
    if ((c >= '0') && (c <= '9')) {
      digit = static_cast<std::uint64_t>(c - '0');
    } else if ((c >= 'a') && (c <= 'f')) {
      digit = static_cast<std::uint64_t>(c - 'a') + 10U;
    } else {
      return false;
    }
    value = (value << 4U) | digit;
  }
  *out = value;
  return true;
}

/// Parses the header line `text` (without its newline). Ok, Corrupt with
/// *reason set, or Unsupported for a newer format version.
SaveReadResult parse_header(const char *text, std::size_t length,
                            SlotHeader *out, const char **reason) noexcept {
  core::JsonParser parser{};
  const core::JsonValue *root = nullptr;
  if (!parser.parse(text, length) || ((root = parser.root()) == nullptr) ||
      (root->type != core::JsonValue::Type::Object)) {
    *reason = "its header is not a JSON object";
    return SaveReadResult::Corrupt;
  }
  core::JsonValue value{};
  const char *format = nullptr;
  std::size_t formatLength = 0U;
  if (!parser.get_object_field(*root, "format", &value) ||
      !parser.as_string(value, &format, &formatLength) ||
      (formatLength != std::strlen(kSlotFormat)) ||
      (std::memcmp(format, kSlotFormat, formatLength) != 0)) {
    *reason = "it is not a save slot file";
    return SaveReadResult::Corrupt;
  }
  std::int64_t version = 0;
  if (!parser.get_object_field(*root, "version", &value) ||
      !parser.as_int64(value, &version) || (version < 1)) {
    *reason = "its header version is not a positive integer";
    return SaveReadResult::Corrupt;
  }
  if (version > static_cast<std::int64_t>(kSaveSlotFormatVersion)) {
    *reason = "a newer build wrote it";
    return SaveReadResult::Unsupported;
  }
  constexpr const char *kKeys[] = {"format", "version", "savedAt",
                                   "payloadBytes", "checksum"};
  constexpr std::size_t kKeyCount = sizeof(kKeys) / sizeof(kKeys[0]);
  bool seen[kKeyCount] = {};
  const std::size_t members = parser.object_size(*root);
  for (std::size_t i = 0U; i < members; ++i) {
    core::JsonValue key{};
    core::JsonValue member{};
    char name[16] = {};
    if (!parser.get_object_member(*root, i, &key, &member) ||
        !parser.copy_string_strict(key, name, sizeof(name))) {
      *reason = "its header has a key it does not define";
      return SaveReadResult::Corrupt;
    }
    std::size_t k = 0U;
    while ((k < kKeyCount) && (std::strcmp(name, kKeys[k]) != 0)) {
      ++k;
    }
    if ((k == kKeyCount) || seen[k]) {
      *reason = "its header has a key it does not define, or one twice";
      return SaveReadResult::Corrupt;
    }
    seen[k] = true;
  }
  std::int64_t savedAt = 0;
  if (!parser.get_object_field(*root, "savedAt", &value) ||
      !parser.as_int64(value, &savedAt) || (savedAt < 0)) {
    *reason = "its save time is not a non-negative integer";
    return SaveReadResult::Corrupt;
  }
  std::uint64_t payloadBytes = 0U;
  if (!parser.get_object_field(*root, "payloadBytes", &value) ||
      !parser.as_uint64(value, &payloadBytes) ||
      (payloadBytes > kSaveSlotCeilingBytes)) {
    *reason = "its payload length is not an integer within the ceiling";
    return SaveReadResult::Corrupt;
  }
  const char *checksumText = nullptr;
  std::size_t checksumLength = 0U;
  std::uint64_t checksum = 0U;
  if (!parser.get_object_field(*root, "checksum", &value) ||
      !parser.as_string(value, &checksumText, &checksumLength) ||
      !parse_checksum(checksumText, checksumLength, &checksum)) {
    *reason = "its checksum is not 16 lower-case hex digits";
    return SaveReadResult::Corrupt;
  }
  out->savedAt = savedAt;
  out->payloadBytes = payloadBytes;
  out->checksum = checksum;
  return SaveReadResult::Ok;
}

/// Logs a read that found the slot file unusable.
void log_read_failure(const char *path, const char *reason) noexcept {
  char message[kPathCapacity + 160] = {};
  std::snprintf(message, sizeof(message), "%.1100s cannot be loaded: %s", path,
                reason);
  core::log_message(core::LogLevel::Error, kLogChannel, message);
}

/// Reads the whole file at `path` (at most `ceiling` bytes) into a fresh
/// buffer; the length in *outLength.
SaveReadResult read_file(const char *path, std::size_t ceiling,
                         std::unique_ptr<char[]> *out,
                         std::size_t *outLength) noexcept {
  std::error_code ec{};
  const std::uintmax_t size =
      std::filesystem::file_size(std::filesystem::path(path), ec);
  if (ec) {
    return path_exists(path) ? SaveReadResult::Unreadable
                             : SaveReadResult::Absent;
  }
  if (size > ceiling) {
    log_read_failure(path, "it is larger than any save this build reads");
    return SaveReadResult::Corrupt;
  }
  const std::size_t capacity = static_cast<std::size_t>(size) + 1U;
  std::unique_ptr<char[]> buffer(new (std::nothrow) char[capacity]);
  if (buffer == nullptr) {
    log_read_failure(path, "there is not enough memory to read it");
    return SaveReadResult::Unreadable;
  }
  std::size_t read = 0U;
  const core::FileReadResult result =
      core::read_whole_file(path, buffer.get(), capacity, &read);
  if (result == core::FileReadResult::Absent) {
    return SaveReadResult::Absent;
  }
  if (result != core::FileReadResult::Ok) {
    log_read_failure(path, "it could not be read whole");
    return SaveReadResult::Unreadable;
  }
  *out = std::move(buffer);
  *outLength = read;
  return SaveReadResult::Ok;
}

/// Counts the slot files in `directory`'s saves folder.
std::size_t count_slots(const char *directory) noexcept {
  return list_game_saves_in(directory, nullptr, 0U);
}

/// Moves a legacy save.json aside after the default slot has a file of its
/// own, so a later discard of that slot cannot bring the old save back.
void retire_legacy_save(const char *directory) noexcept {
  char legacy[kPathCapacity] = {};
  if (!legacy_path(directory, legacy, sizeof(legacy)) || !path_exists(legacy)) {
    return;
  }
  char target[kPathCapacity + 32] = {};
  char message[kPathCapacity * 2] = {};
  if (move_aside(legacy, "migrated", target, sizeof(target))) {
    std::snprintf(message, sizeof(message),
                  "the default save slot replaced the old save; it was moved "
                  "aside to %.1100s",
                  target);
    core::log_message(core::LogLevel::Info, kLogChannel, message);
  } else {
    std::snprintf(message, sizeof(message),
                  "%.1100s could not be moved aside after the default slot "
                  "was saved; the slot is read in its place",
                  legacy);
    core::log_message(core::LogLevel::Warning, kLogChannel, message);
  }
}

} // namespace

bool save_slot_name_is_valid(const char *slot) noexcept {
  return core::name_token_is_valid(slot, kSaveSlotNameCapacity - 1U);
}

bool set_save_slot_limit(std::size_t bytes) noexcept {
  if ((bytes == 0U) || (bytes > kSaveSlotCeilingBytes)) {
    return false;
  }
  g_slotLimit = bytes;
  return true;
}

std::size_t save_slot_limit() noexcept { return g_slotLimit; }

void hold_game_save_in(const char *directory, const char *slot) noexcept {
  char lower[kSaveSlotNameCapacity] = {};
  char path[kPathCapacity] = {};
  if ((directory == nullptr) || !lower_slot(slot, lower) ||
      !slot_path(directory, lower, path, sizeof(path))) {
    // A slot path that does not fit is one no save can write either.
    return;
  }
  if (held_index(path) >= 0) {
    return;
  }
  for (std::size_t i = 0U; i < kMaxHeldSlots; ++i) {
    if (g_held[i][0] == '\0') {
      std::memcpy(g_held[i].data(), path, std::strlen(path) + 1U);
      return;
    }
  }
  g_heldOverflow = true;
  core::log_message(core::LogLevel::Error, kLogChannel,
                    "too many save slots could not be loaded to remember "
                    "each; every save is refused until the game restarts");
}

bool game_save_held_in(const char *directory, const char *slot) noexcept {
  char lower[kSaveSlotNameCapacity] = {};
  char path[kPathCapacity] = {};
  if ((directory == nullptr) || !lower_slot(slot, lower) ||
      !slot_path(directory, lower, path, sizeof(path))) {
    return false;
  }
  return g_heldOverflow || (held_index(path) >= 0);
}

bool discard_game_save_in(const char *directory, const char *slot) noexcept {
  char lower[kSaveSlotNameCapacity] = {};
  char path[kPathCapacity] = {};
  if (directory == nullptr) {
    return false;
  }
  if (!lower_slot(slot, lower)) {
    log_invalid_slot(slot);
    return false;
  }
  if (!slot_path(directory, lower, path, sizeof(path))) {
    return false;
  }
  char legacy[kPathCapacity] = {};
  const bool hasLegacy = (std::strcmp(lower, kDefaultSaveSlot) == 0) &&
                         legacy_path(directory, legacy, sizeof(legacy)) &&
                         path_exists(legacy);
  const char *files[2] = {path_exists(path) ? path : nullptr,
                          hasLegacy ? legacy : nullptr};
  char target[kPathCapacity + 32] = {};
  char message[kPathCapacity * 2] = {};
  for (const char *file : files) {
    if (file == nullptr) {
      continue;
    }
    if (!move_aside(file, "discarded", target, sizeof(target))) {
      core::log_message(core::LogLevel::Error, kLogChannel,
                        "the save could not be moved aside; it is kept and "
                        "new saves to its slot are still refused");
      return false;
    }
    std::snprintf(message, sizeof(message),
                  "the save was moved aside to %.1100s; the next save starts "
                  "a new file",
                  target);
    core::log_message(core::LogLevel::Info, kLogChannel, message);
  }
  const int index = held_index(path);
  if (index >= 0) {
    g_held[static_cast<std::size_t>(index)][0] = '\0';
  }
  return true;
}

bool save_game_data_to(const char *directory, const char *slot,
                       const char *payload, std::size_t length) noexcept {
  if ((directory == nullptr) || (payload == nullptr)) {
    return false;
  }
  char lower[kSaveSlotNameCapacity] = {};
  if (!lower_slot(slot, lower)) {
    log_invalid_slot(slot);
    return false;
  }
  if (length > g_slotLimit) {
    char message[200] = {};
    std::snprintf(message, sizeof(message),
                  "save of %zu bytes exceeds the project's %zu-byte save "
                  "slot limit; the previous save is unchanged",
                  length, g_slotLimit);
    core::log_message(core::LogLevel::Error, kLogChannel, message);
    return false;
  }
  char path[kPathCapacity] = {};
  char folder[kPathCapacity] = {};
  if (!slot_path(directory, lower, path, sizeof(path)) ||
      !slot_directory(directory, folder, sizeof(folder))) {
    core::log_message(core::LogLevel::Error, kLogChannel,
                      "save path exceeds the buffer");
    return false;
  }
  if (g_heldOverflow || (held_index(path) >= 0)) {
    core::log_message(core::LogLevel::Error, kLogChannel,
                      "save refused: the save on disk could not be loaded "
                      "and may be the only copy of the player's progress; it "
                      "is kept until it is discarded (engine.discard_save in "
                      "Lua), which moves it aside");
    return false;
  }
  if (!path_exists(path) && (count_slots(directory) >= kMaxSaveSlots)) {
    char message[160] = {};
    std::snprintf(message, sizeof(message),
                  "save refused: the project already has %zu save slots; "
                  "discard one before saving a new one",
                  kMaxSaveSlots);
    core::log_message(core::LogLevel::Error, kLogChannel, message);
    return false;
  }

  // The folder does not exist before a profile's first save, and one
  // created here has its own entry synced: the save's durability would
  // otherwise rest on a directory that might not survive the same power
  // loss.
  if (!core::create_directories_durably(folder)) {
    core::log_message(core::LogLevel::Error, kLogChannel,
                      "failed to create the save directory");
    return false;
  }
  char header[kMaxHeaderBytes] = {};
  const int headerLength =
      std::snprintf(header, sizeof(header),
                    "{\"format\":\"%s\",\"version\":%u,\"savedAt\":%lld,"
                    "\"payloadBytes\":%zu,\"checksum\":\"%016" PRIx64 "\"}\n",
                    kSlotFormat, kSaveSlotFormatVersion,
                    static_cast<long long>(std::time(nullptr)), length,
                    payload_checksum(payload, length));
  if ((headerLength <= 0) ||
      (static_cast<std::size_t>(headerLength) >= sizeof(header))) {
    return false;
  }
  core::AtomicFileWriter writer{};
  if (!writer.begin(path) ||
      !writer.write(header, static_cast<std::size_t>(headerLength)) ||
      !writer.write(payload, length) || !writer.commit()) {
    core::log_message(core::LogLevel::Error, kLogChannel,
                      "failed to write the save file; the previous save is "
                      "unchanged");
    return false;
  }
  if (std::strcmp(lower, kDefaultSaveSlot) == 0) {
    retire_legacy_save(directory);
  }
  return true;
}

SaveReadResult read_game_data_from(const char *directory, const char *slot,
                                   SaveSlotPayload *out) noexcept {
  if (out != nullptr) {
    *out = SaveSlotPayload{};
  }
  if ((directory == nullptr) || (out == nullptr)) {
    return SaveReadResult::Unreadable;
  }
  char lower[kSaveSlotNameCapacity] = {};
  if (!lower_slot(slot, lower)) {
    log_invalid_slot(slot);
    return SaveReadResult::Unreadable;
  }
  char path[kPathCapacity] = {};
  if (!slot_path(directory, lower, path, sizeof(path))) {
    return SaveReadResult::Unreadable;
  }

  std::unique_ptr<char[]> buffer;
  std::size_t length = 0U;
  SaveReadResult result = read_file(
      path, kSaveSlotCeilingBytes + kMaxHeaderBytes, &buffer, &length);
  if ((result == SaveReadResult::Absent) &&
      (std::strcmp(lower, kDefaultSaveSlot) == 0)) {
    // A save from before slots existed is the default slot's payload,
    // whole and unchecked, until the slot is next saved.
    char legacy[kPathCapacity] = {};
    if (!legacy_path(directory, legacy, sizeof(legacy))) {
      return SaveReadResult::Absent;
    }
    result = read_file(legacy, kSaveSlotCeilingBytes, &buffer, &length);
    if (result == SaveReadResult::Ok) {
      out->storage = std::move(buffer);
      out->data = out->storage.get();
      out->length = length;
    }
    return result;
  }
  if (result != SaveReadResult::Ok) {
    return result;
  }

  const char *text = buffer.get();
  const std::size_t scan =
      (length < kMaxHeaderBytes) ? length : kMaxHeaderBytes;
  const void *newline = std::memchr(text, '\n', scan);
  if (newline == nullptr) {
    log_read_failure(path, "it has no header line");
    return SaveReadResult::Corrupt;
  }
  const std::size_t headerLength =
      static_cast<std::size_t>(static_cast<const char *>(newline) - text);
  SlotHeader header{};
  const char *reason = "";
  result = parse_header(text, headerLength, &header, &reason);
  if (result != SaveReadResult::Ok) {
    log_read_failure(path, reason);
    return result;
  }
  const std::size_t payloadOffset = headerLength + 1U;
  const std::size_t payloadLength = length - payloadOffset;
  if (header.payloadBytes != payloadLength) {
    log_read_failure(path, (payloadLength < header.payloadBytes)
                               ? "it is shorter than its header says, so it "
                                 "was cut off"
                               : "it holds bytes past the payload its "
                                 "header describes");
    return SaveReadResult::Corrupt;
  }
  if (payload_checksum(text + payloadOffset, payloadLength) !=
      header.checksum) {
    log_read_failure(path, "its checksum does not match, so its bytes "
                           "changed after it was written");
    return SaveReadResult::Corrupt;
  }
  out->storage = std::move(buffer);
  out->data = out->storage.get() + payloadOffset;
  out->length = payloadLength;
  return SaveReadResult::Ok;
}

std::size_t list_game_saves_in(const char *directory, SaveSlotInfo *out,
                               std::size_t capacity) noexcept {
  if (directory == nullptr) {
    return 0U;
  }
  if (out == nullptr) {
    capacity = 0U;
  }
  char folder[kPathCapacity] = {};
  std::size_t total = 0U;
  // Keeps the `capacity` lowest names in order, so the listing is the same
  // whatever order the filesystem returns entries in.
  const auto insert = [&](const SaveSlotInfo &info) noexcept {
    ++total;
    std::size_t kept = (total - 1U < capacity) ? total - 1U : capacity;
    std::size_t at = 0U;
    while ((at < kept) && (std::strcmp(out[at].slot, info.slot) < 0)) {
      ++at;
    }
    if (at >= capacity) {
      return;
    }
    const std::size_t last = (kept < capacity) ? kept : capacity - 1U;
    for (std::size_t i = last; i > at; --i) {
      out[i] = out[i - 1U];
    }
    out[at] = info;
  };

  bool hasDefault = false;
  if (slot_directory(directory, folder, sizeof(folder))) {
    std::error_code ec{};
    std::filesystem::directory_iterator it(std::filesystem::path(folder), ec);
    const std::filesystem::directory_iterator end{};
    for (; !ec && (it != end); it.increment(ec)) {
      const std::filesystem::path &entry = it->path();
      if (entry.extension() != kSlotExtension) {
        continue;
      }
      const std::string stem = entry.stem().string();
      SaveSlotInfo info{};
      if (!save_slot_name_is_valid(stem.c_str())) {
        continue;
      }
      bool lowerCase = true;
      for (const char c : stem) {
        lowerCase = lowerCase && (core::ascii_lower(c) == c);
      }
      if (!lowerCase) {
        // No save writes such a name, so no read finds it either.
        continue;
      }
      std::memcpy(info.slot, stem.c_str(), stem.size() + 1U);
      hasDefault = hasDefault || (stem == kDefaultSaveSlot);
      if (capacity == 0U) {
        // Counting only: no header needs reading.
        insert(info);
        continue;
      }
      char headerText[kMaxHeaderBytes] = {};
      std::size_t headerRead = 0U;
      const std::string file = entry.string();
      if (core::read_file_prefix(file.c_str(), headerText, sizeof(headerText),
                                 &headerRead) != core::FileReadResult::Ok) {
        info.status = SaveReadResult::Unreadable;
      } else {
        const void *newline = std::memchr(headerText, '\n', headerRead);
        SlotHeader header{};
        const char *reason = "";
        info.status =
            (newline == nullptr)
                ? SaveReadResult::Corrupt
                : parse_header(
                      headerText,
                      static_cast<std::size_t>(
                          static_cast<const char *>(newline) - headerText),
                      &header, &reason);
        if (info.status == SaveReadResult::Ok) {
          info.savedAt = header.savedAt;
          info.payloadBytes = header.payloadBytes;
        }
      }
      insert(info);
    }
  }
  char legacy[kPathCapacity] = {};
  if (!hasDefault && legacy_path(directory, legacy, sizeof(legacy))) {
    std::error_code ec{};
    const std::uintmax_t size =
        std::filesystem::file_size(std::filesystem::path(legacy), ec);
    if (!ec) {
      SaveSlotInfo info{};
      std::memcpy(info.slot, kDefaultSaveSlot,
                  std::strlen(kDefaultSaveSlot) + 1U);
      info.payloadBytes = static_cast<std::uint64_t>(size);
      info.legacy = true;
      insert(info);
    }
  }
  return total;
}

bool save_game_data(const char *slot, const char *payload,
                    std::size_t length) noexcept {
  char directory[1024] = {};
  if (!core::project_data_dir(directory, sizeof(directory))) {
    core::log_message(core::LogLevel::Error, kLogChannel,
                      "project save directory unavailable");
    return false;
  }
  return save_game_data_to(directory, slot, payload, length);
}

SaveReadResult read_game_data(const char *slot, SaveSlotPayload *out) noexcept {
  if (out != nullptr) {
    *out = SaveSlotPayload{};
  }
  char directory[1024] = {};
  if (!core::project_data_dir(directory, sizeof(directory))) {
    return SaveReadResult::Absent;
  }
  const SaveReadResult result = read_game_data_from(directory, slot, out);
  if (result == SaveReadResult::Absent) {
    note_unattributed_legacy_save();
  }
  return result;
}

std::size_t list_game_saves(SaveSlotInfo *out, std::size_t capacity) noexcept {
  char directory[1024] = {};
  if (!core::project_data_dir(directory, sizeof(directory))) {
    return 0U;
  }
  return list_game_saves_in(directory, out, capacity);
}

void hold_game_save(const char *slot) noexcept {
  char directory[1024] = {};
  if (core::project_data_dir(directory, sizeof(directory))) {
    hold_game_save_in(directory, slot);
  }
}

bool discard_game_save(const char *slot) noexcept {
  char directory[1024] = {};
  if (!core::project_data_dir(directory, sizeof(directory))) {
    core::log_message(core::LogLevel::Error, kLogChannel,
                      "project save directory unavailable");
    return false;
  }
  return discard_game_save_in(directory, slot);
}

} // namespace engine::runtime
