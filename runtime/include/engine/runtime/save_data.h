// Declares the game's save slots (Unreal's SaveGameToSlot): named
// documents in the project's data directory (engine/core/project_data.h),
// one file per slot under saves/, holding what a game keeps between runs
// (inventory, quest flags, world state, settings). Each file is a one-line
// header (format version, save time, payload length and checksum) followed
// by the game's payload, so a torn or damaged file is detected rather than
// read as a shorter save. The largest slot a game may write is a project
// setting; a save.json from before slots existed reads as the "default"
// slot until the game next saves it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace engine::runtime {

/// The largest slot a game writes while its project sets no bound.
inline constexpr std::size_t kDefaultSaveSlotLimitBytes = 4U * 1024U * 1024U;
/// The largest payload any slot read accepts and the top of the range a
/// project may set, so lowering a project's bound never locks a player
/// out of a save an earlier build wrote.
inline constexpr std::size_t kSaveSlotCeilingBytes = 256U * 1024U * 1024U;
/// A slot name's capacity, terminator included.
inline constexpr std::size_t kSaveSlotNameCapacity = 32U;
/// How many slots a project's directory holds; a save to a new slot past
/// it is refused.
inline constexpr std::size_t kMaxSaveSlots = 256U;
/// The slot a save without a slot name goes to.
inline constexpr const char *kDefaultSaveSlot = "default";
/// The slot header's format version. A header naming a newer one came
/// from a newer build and is not read.
inline constexpr std::uint32_t kSaveSlotFormatVersion = 1U;

/// True when `slot` is a slot name: a name token (letters, CJK, kana and
/// Hangul included, digits, '_', '-' and '.', 1 to 31 bytes). Slot names ignore
/// case: "Hero" and "hero" are one slot, stored lower-case, so a slot is the
/// same file on every filesystem.
bool save_slot_name_is_valid(const char *slot) noexcept;

/// Sets the largest payload a save writes; false, with the bound
/// unchanged, outside [1, kSaveSlotCeilingBytes]. Bootstrap sets it from
/// the project's "saves" setting.
bool set_save_slot_limit(std::size_t bytes) noexcept;

/// The largest payload a save writes.
std::size_t save_slot_limit() noexcept;

/// What reading a slot found. Absent is the ordinary first run. Unreadable
/// is a failed read; Corrupt a file that is not a whole, intact save
/// (a bad header, a payload shorter or longer than its header says, or a
/// checksum that does not match); Unsupported a save a newer build wrote.
enum class SaveReadResult : std::uint8_t {
  Ok,
  Absent,
  Unreadable,
  Corrupt,
  Unsupported,
};

/// A slot's payload as read: `length` bytes at `data`, NUL-terminated,
/// owned by `storage`.
struct SaveSlotPayload final {
  std::unique_ptr<char[]> storage;
  const char *data = nullptr;
  std::size_t length = 0U;
};

/// One slot as a listing sees it, from its header alone.
struct SaveSlotInfo final {
  /// The slot's name, lower-case.
  char slot[kSaveSlotNameCapacity] = {};
  /// Ok, or what is wrong with its header (Corrupt, Unsupported,
  /// Unreadable); the payload is checked only when the slot is read.
  SaveReadResult status = SaveReadResult::Ok;
  /// Seconds since 1970-01-01 UTC when it was saved; 0 for a legacy save
  /// or a header that could not be read.
  std::int64_t savedAt = 0;
  /// The payload's size in bytes.
  std::uint64_t payloadBytes = 0U;
  /// True for a save.json from before slots existed, listed as "default".
  bool legacy = false;
};

/// Writes `payload` as `slot` in `directory` (saves/<slot>.save, the
/// directory created when missing) through a staged atomic replace. False,
/// logged, with the slot's previous file unchanged, for an invalid slot
/// name, a payload over save_slot_limit(), a held slot, a new slot past
/// kMaxSaveSlots, or a failed write. The first save of the default slot
/// moves a legacy save.json aside to save.json.migrated-<n>. Tests use
/// this to avoid the real per-user directory.
bool save_game_data_to(const char *directory, const char *slot,
                       const char *payload, std::size_t length) noexcept;

/// Reads `slot` from `directory` into `out`, checking its header, length
/// and checksum; `out` is emptied first and filled only on Ok. Reading the
/// default slot while it has no file reads a legacy save.json whole. Every
/// result but Ok and Absent is logged. The caller that cannot use what it
/// read holds the slot (hold_game_save_in) so the next save cannot
/// replace it.
SaveReadResult read_game_data_from(const char *directory, const char *slot,
                                   SaveSlotPayload *out) noexcept;

/// Lists the slots in `directory`, sorted by name, into `out` (at most
/// `capacity`) and returns how many there are, which may exceed
/// `capacity`. Only headers are read, so listing many large slots is
/// cheap.
std::size_t list_game_saves_in(const char *directory, SaveSlotInfo *out,
                               std::size_t capacity) noexcept;

/// Holds `slot` in `directory`: a load found it unreadable, corrupt or
/// written by a newer build, so it may be the only copy of a player's
/// progress. Until discard_game_save_in, every save to that slot is
/// refused with a logged Error and the file stays as it is; other slots
/// save normally.
void hold_game_save_in(const char *directory, const char *slot) noexcept;

/// True while `slot` in `directory` is held.
bool game_save_held_in(const char *directory, const char *slot) noexcept;

/// Moves a held or unwanted slot aside to the first free
/// <slot>.save.discarded-<n> (n from 1), never deleting it, and lifts the
/// hold so the next save starts a new file. Discarding the default slot
/// moves a legacy save.json aside too. True when there was nothing to move
/// or it moved; false, logged, with the hold kept, when a move fails.
bool discard_game_save_in(const char *directory, const char *slot) noexcept;

/// save_game_data_to in the project's data directory; false, logged, when
/// no project is named.
bool save_game_data(const char *slot, const char *payload,
                    std::size_t length) noexcept;

/// read_game_data_from in the project's data directory. A save.json left
/// in the shared per-user directory by an engine that predates per-project
/// saves is never read, since nothing ties it to this project; its
/// presence is logged once. No project reads as Absent.
SaveReadResult read_game_data(const char *slot, SaveSlotPayload *out) noexcept;

/// list_game_saves_in the project's data directory; 0 with no project.
std::size_t list_game_saves(SaveSlotInfo *out, std::size_t capacity) noexcept;

/// hold_game_save_in for the project's data directory.
void hold_game_save(const char *slot) noexcept;

/// discard_game_save_in for the project's data directory; false, logged,
/// when no project is named.
bool discard_game_save(const char *slot) noexcept;

} // namespace engine::runtime
