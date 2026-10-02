// Starts the audio device on a platform where audio waits for the user's
// first gesture (PlatformCaps::needsAudioUnlock). The engine is
// initialized with noAutoStart there, and the platform's own translation
// unit, chosen in CMake, prepares the device for that wait and starts it.

#pragma once

#include "miniaudio_include.h"

namespace engine::audio {

/// Starts `engine`'s device after the platform has prepared it for the
/// unlock; false when the device refuses to start.
bool start_device_awaiting_unlock(ma_engine *engine) noexcept;

} // namespace engine::audio
