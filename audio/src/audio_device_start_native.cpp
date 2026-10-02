// Starts the audio device where no platform preparation is needed. No
// native desktop platform awaits an audio unlock, so the engine reaches
// this only if a platform's caps say it must.

#include "audio_device_start.h"

namespace engine::audio {

bool start_device_awaiting_unlock(ma_engine *engine) noexcept {
  return ma_engine_start(engine) == MA_SUCCESS;
}

} // namespace engine::audio
