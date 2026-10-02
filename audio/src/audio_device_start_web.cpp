// Starts the audio device in a browser: miniaudio's WebAudio context
// stays suspended until the page's first user gesture, so the resume it
// issues is observed before the device starts. Compiled only for the web.

#include "audio_device_start.h"

#include <emscripten.h>

namespace engine::audio {

namespace {

/// miniaudio's WebAudio start calls AudioContext.resume() and drops the
/// promise. It stays pending until the page's first user gesture, and a
/// shutdown in between closes the context under it, which rejects it as an
/// unhandled "Cannot resume a closed AudioContext". Observing every resume
/// on this device's context settles that case quietly; a resume that fails
/// while the context is still open is still reported.
void observe_webaudio_resume(const ma_device &device) noexcept {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdollar-in-identifier-extension"
#endif
  // The block is JavaScript, which the formatter would rewrite (it splits
  // `!==`), so it stays as written.
  // clang-format off
  EM_ASM(
      {
        var context = miniaudio.get_device_by_index($0).webaudio;
        var resume = context.resume.bind(context);
        context.resume = function() {
          var pending = resume();
          pending.catch(function(error) {
            if (context.state !== 'closed') {
              console.error('audio: AudioContext resume failed', error);
            }
          });
          return pending;
        };
      },
      device.webaudio.deviceIndex);
  // clang-format on
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
}

} // namespace

bool start_device_awaiting_unlock(ma_engine *engine) noexcept {
  observe_webaudio_resume(*ma_engine_get_device(engine));
  return ma_engine_start(engine) == MA_SUCCESS;
}

} // namespace engine::audio
