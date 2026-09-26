// Declares the single-slot game save: a JSON document written to the
// project's data directory (engine/core/project_data.h), one per project
// per user (best times, progress flags — the small key/value data the
// v0.1 templates persist between runs).

#pragma once

#include <cstddef>

namespace engine::runtime {

inline constexpr std::size_t kMaxSaveDataBytes = 16U * 1024U;

/// Writes the JSON document to the given directory as save.json
/// (directory created when missing); false on IO failure or oversized
/// input. Tests use this to avoid the real per-user directory.
bool save_game_data_to(const char *directory, const char *json,
                       std::size_t length) noexcept;

/// Reads save.json from the given directory into out (null-terminated);
/// false when absent, larger than the capacity, or when the read itself
/// fails — a failed read is never reported as a shorter document.
bool load_game_data_from(const char *directory, char *out,
                         std::size_t capacity,
                         std::size_t *outLength) noexcept;

/// Writes the save slot to the project's data directory; false, logged,
/// when no project is named.
bool save_game_data(const char *json, std::size_t length) noexcept;

/// Reads the save slot from the project's data directory. A save.json left
/// in the shared per-user directory by an engine that predates per-project
/// saves is never read, since nothing ties it to this project; its
/// presence is logged once.
bool load_game_data(char *out, std::size_t capacity,
                    std::size_t *outLength) noexcept;

} // namespace engine::runtime
