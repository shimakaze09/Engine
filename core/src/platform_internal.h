// Private declarations shared by the platform layer's translation units:
// platform.cpp (SDL lifecycle, window, events), platform_file_dialogs.cpp
// (native and scripted dialogs) and the per-OS platform_os_*.cpp files
// (services that do not touch SDL). Only state and helpers that more than
// one of them needs live here; everything else stays file-local. The header
// names no SDL, bgfx, Lua or ImGui types, so the OS files include it without
// reaching SDL.

#pragma once

#include <cstddef>

struct SDL_Window;

namespace engine::core::platform_detail {

/// Longest path, in bytes with the terminator, the platform path helpers
/// build.
inline constexpr std::size_t kPlatformPathMax = 1024U;

/// The one application window, or null before initialization and after
/// shutdown. Main thread only; platform.cpp creates and destroys it, and
/// the file dialogs parent themselves to it.
inline SDL_Window *g_window = nullptr;

/// Logs `message` at error level, appending SDL's last error text when it
/// has one. Defined in platform.cpp, the layer's SDL owner.
void log_sdl_error(const char *message) noexcept;

/// Empties `outBuffer` and returns whether it can hold a result at all.
bool validate_path_output(char *outBuffer, std::size_t bufferCapacity) noexcept;

/// Copies `path` into `outBuffer` with separators and any trailing slash
/// normalized. False, with the buffer emptied, for an empty path or a
/// buffer too small to hold it.
bool copy_normalized_path(const char *path, char *outBuffer,
                          std::size_t bufferCapacity) noexcept;

/// Appends `segment` to the normalized path in `base`, adding a separator
/// when one is missing. False, leaving `base` unextended, when it does not
/// fit in `capacity`.
bool append_path_segment(char *base, std::size_t capacity,
                         const char *segment) noexcept;

/// Builds the per-user data root the save directory lives under. Defined by
/// the OS file CMake selects for the target.
bool build_save_base(char *outBuffer, std::size_t bufferCapacity) noexcept;

} // namespace engine::core::platform_detail
