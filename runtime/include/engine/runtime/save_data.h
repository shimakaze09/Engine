// Declares the single-slot game save: a JSON document written to the
// project's data directory (engine/core/project_data.h), one per project
// per user, holding what a game keeps between runs (inventory, quest
// flags, world state, settings) up to a documented hard ceiling.

#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::runtime {

/// The hard ceiling on one save document. Saves are a cold path, so the
/// bound is set by what a game stores rather than by a preallocated
/// buffer; a document past it is refused whole, never truncated.
inline constexpr std::size_t kMaxSaveDataBytes = 4U * 1024U * 1024U;

/// Writes the JSON document to the given directory as save.json
/// (directory created when missing) through a staged atomic replace;
/// false on IO failure, a document over kMaxSaveDataBytes or a held slot,
/// logged, with the previous save.json unchanged. Tests use this to avoid the
/// real per-user directory.
bool save_game_data_to(const char *directory, const char *json,
                       std::size_t length) noexcept;

/// What reading the save slot found: Absent is the ordinary first run,
/// Unreadable a failed read or a file past the capacity.
enum class SaveReadResult : std::uint8_t { Ok, Absent, Unreadable };

/// Reads save.json from the given directory into out (null-terminated).
/// A failed read is never reported as a shorter document: it is
/// Unreadable, logged. The caller that cannot use what it read holds the
/// slot (hold_game_save_in) so the next save cannot replace it.
SaveReadResult read_game_data_from(const char *directory, char *out,
                                   std::size_t capacity,
                                   std::size_t *outLength) noexcept;

/// read_game_data_from, true only for Ok.
bool load_game_data_from(const char *directory, char *out,
                         std::size_t capacity,
                         std::size_t *outLength) noexcept;

/// Holds the slot in `directory`: a load found its save.json unreadable,
/// corrupt or written by a newer build, so it may be the only copy of a
/// player's progress. Until discard_game_save_in, every save to that
/// directory is refused with a logged Error and the file stays as it is.
/// The format reader calls this for a slot it could not read or a
/// document it cannot accept.
void hold_game_save_in(const char *directory) noexcept;

/// True while the slot in `directory` is held.
bool game_save_held_in(const char *directory) noexcept;

/// Moves a held or unwanted save.json aside to the first free
/// save.json.discarded-<n> (n from 1), never deleting it, and lifts the
/// hold so the next save starts a new file. True when there was no file
/// or it moved; false, logged, with the hold kept, when the move fails.
bool discard_game_save_in(const char *directory) noexcept;

/// Writes the save slot to the project's data directory; false, logged,
/// when no project is named.
bool save_game_data(const char *json, std::size_t length) noexcept;

/// Reads the save slot from the project's data directory. A save.json left
/// in the shared per-user directory by an engine that predates per-project
/// saves is never read, since nothing ties it to this project; its
/// presence is logged once. No project reads as Absent.
SaveReadResult read_game_data(char *out, std::size_t capacity,
                              std::size_t *outLength) noexcept;

/// read_game_data, true only for Ok.
bool load_game_data(char *out, std::size_t capacity,
                    std::size_t *outLength) noexcept;

/// hold_game_save_in for the project's data directory.
void hold_game_save() noexcept;

/// discard_game_save_in for the project's data directory; false, logged,
/// when no project is named.
bool discard_game_save() noexcept;

} // namespace engine::runtime
