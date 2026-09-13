// Verifies the Lua audio bindings share the binding layer's one numeric-
// argument contract (#481): every float argument must be a finite number.
// A NaN or infinite volume, pitch, or one-shot position fails the call
// (false, or no effect for the result-less set_bus_volume) before the
// runtime audio service is reached; an omitted optional still takes its
// documented default. The runtime service is a recorder here, so the test
// observes exactly which calls crossed the bridge and with what values.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "../test_harness.h"
#include "engine/core/service_locator.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace {

namespace sc = engine::scripting;
namespace rt = engine::runtime;

constexpr const char *kScriptPath = "script_audio_finite_args_test.lua";

/// Writes contents to the temporary test script path.
bool write_script_file(const char *contents) noexcept {
  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, kScriptPath, "wb") != 0 || file == nullptr) {
    return false;
  }
#else
  file = std::fopen(kScriptPath, "wb");
  if (file == nullptr) {
    return false;
  }
#endif
  const std::size_t len = std::strlen(contents);
  const bool ok = (std::fwrite(contents, 1U, len, file) == len);
  std::fclose(file);
  return ok;
}

/// Last call each recorded audio service saw, plus its call count.
struct AudioRecorder final {
  unsigned playSoundCalls = 0U;
  std::uint32_t playSoundId = 0U;
  float playSoundVolume = 0.0F;
  float playSoundPitch = 0.0F;
  bool playSoundLoop = false;

  unsigned playSoundAtCalls = 0U;
  float playSoundAtX = 0.0F;
  float playSoundAtY = 0.0F;
  float playSoundAtZ = 0.0F;
  float playSoundAtVolume = 0.0F;

  unsigned setBusVolumeCalls = 0U;
  std::uint32_t setBusVolumeBus = 0U;
  float setBusVolumeValue = 0.0F;

  unsigned playMusicCalls = 0U;
  float playMusicVolume = 0.0F;
  bool playMusicLoop = false;
};

AudioRecorder g_recorder{};

bool record_play_sound(std::uint32_t soundId, float volume, float pitch,
                       bool loop) noexcept {
  ++g_recorder.playSoundCalls;
  g_recorder.playSoundId = soundId;
  g_recorder.playSoundVolume = volume;
  g_recorder.playSoundPitch = pitch;
  g_recorder.playSoundLoop = loop;
  return true;
}

bool record_play_sound_at(std::uint32_t /*soundId*/, float x, float y,
                          float z, float volume) noexcept {
  ++g_recorder.playSoundAtCalls;
  g_recorder.playSoundAtX = x;
  g_recorder.playSoundAtY = y;
  g_recorder.playSoundAtZ = z;
  g_recorder.playSoundAtVolume = volume;
  return true;
}

void record_set_bus_volume(std::uint32_t bus, float volume) noexcept {
  ++g_recorder.setBusVolumeCalls;
  g_recorder.setBusVolumeBus = bus;
  g_recorder.setBusVolumeValue = volume;
}

bool record_play_music(const char * /*path*/, float volume,
                       bool loop) noexcept {
  ++g_recorder.playMusicCalls;
  g_recorder.playMusicVolume = volume;
  g_recorder.playMusicLoop = loop;
  return true;
}

// Each rejection row: the binding returns false and the recorder sees no
// call. Each acceptance row: the recorder sees the call and the defaults.
// A row that fails raises, so call_script_function reports it as false.
constexpr const char *kScript =
    "local nan = 0/0\n"
    "local inf = math.huge\n"
    "local function expect(cond, what)\n"
    "    if not cond then error(what) end\n"
    "end\n"
    "function reject_play_sound_volume()\n"
    "    expect(engine.play_sound(1, nan) == false, 'nan volume accepted')\n"
    "end\n"
    "function reject_play_sound_pitch()\n"
    "    expect(engine.play_sound(1, 0.5, inf) == false, 'inf pitch accepted')\n"
    "end\n"
    "function reject_play_sound_volume_type()\n"
    "    expect(engine.play_sound(1, 'loud') == false, 'string volume accepted')\n"
    "end\n"
    "function accept_play_sound_defaults()\n"
    "    expect(engine.play_sound(7) == true, 'defaults refused')\n"
    "end\n"
    "function accept_play_sound_nil_optionals()\n"
    "    expect(engine.play_sound(7, nil, nil, true) == true, 'nils refused')\n"
    "end\n"
    "function accept_play_sound_values()\n"
    "    expect(engine.play_sound(7, 0.25, 1.5, false) == true, 'values refused')\n"
    "end\n"
    "function reject_play_sound_at_position()\n"
    "    expect(engine.play_sound_at(1, 0, nan, 0) == false, 'nan y accepted')\n"
    "end\n"
    "function reject_play_sound_at_volume()\n"
    "    expect(engine.play_sound_at(1, 0, 0, 0, -inf) == false,\n"
    "           '-inf volume accepted')\n"
    "end\n"
    "function accept_play_sound_at_defaults()\n"
    "    expect(engine.play_sound_at(1, 1, 2, 3) == true, 'default refused')\n"
    "end\n"
    "function accept_play_sound_at_volume()\n"
    "    expect(engine.play_sound_at(1, 1, 2, 3, 0.75) == true, 'volume refused')\n"
    "end\n"
    "function reject_bus_volume()\n"
    "    engine.set_bus_volume('sfx', nan)\n"
    "    engine.set_bus_volume('music', inf)\n"
    "end\n"
    "function accept_bus_volume()\n"
    "    engine.set_bus_volume('sfx', 0.5)\n"
    "end\n"
    "function reject_play_music_volume()\n"
    "    expect(engine.play_music('sounds/track.wav', nan) == false,\n"
    "           'nan music volume accepted')\n"
    "end\n"
    "function accept_play_music_defaults()\n"
    "    expect(engine.play_music('sounds/track.wav') == true, 'default refused')\n"
    "end\n"
    "function accept_play_music_values()\n"
    "    expect(engine.play_music('sounds/track.wav', 0.25, false) == true,\n"
    "           'values refused')\n"
    "end\n";

/// Runs a script function that raises on a failed expectation.
bool run_verdict(const char *name) noexcept {
  return sc::call_script_function(name);
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!sc::initialize_scripting()) {
    std::fprintf(stderr, "FAIL: initialize_scripting\n");
    return 1;
  }

  auto world = std::unique_ptr<rt::World>(new (std::nothrow) rt::World());
  if (world == nullptr) {
    sc::shutdown_scripting();
    return 1;
  }
  engine::core::ServiceLocator serviceLocator{};
  rt::bind_scripting_runtime(world.get(), serviceLocator);

  // The production bridge publishes the runtime's audio services; the
  // recorder replaces only those so the bindings under test are the
  // production ones and the observation point is the bridge boundary.
  sc::RuntimeServices services =
      *serviceLocator.get_service<sc::RuntimeServices>();
  services.play_sound = &record_play_sound;
  services.play_sound_at = &record_play_sound_at;
  services.set_bus_volume = &record_set_bus_volume;
  services.play_music = &record_play_music;
  sc::bind_runtime_services(&services, serviceLocator);

  engine::tests::TestContext ctx;
  ctx.check(write_script_file(kScript), "write test script");
  ctx.check(sc::load_script(kScriptPath), "load test script");

  g_recorder = AudioRecorder{};
  ctx.check(run_verdict("reject_play_sound_volume"),
            "play_sound: NaN volume returns false");
  ctx.check(run_verdict("reject_play_sound_pitch"),
            "play_sound: infinite pitch returns false");
  ctx.check(run_verdict("reject_play_sound_volume_type"),
            "play_sound: non-number volume returns false");
  ctx.check(g_recorder.playSoundCalls == 0U,
            "play_sound: rejected arguments never reach the service");

  ctx.check(run_verdict("accept_play_sound_defaults"),
            "play_sound: omitted optionals accepted");
  ctx.check((g_recorder.playSoundCalls == 1U) &&
                (g_recorder.playSoundId == 7U) &&
                (g_recorder.playSoundVolume == 1.0F) &&
                (g_recorder.playSoundPitch == 1.0F) &&
                !g_recorder.playSoundLoop,
            "play_sound: omitted optionals take volume 1, pitch 1, no loop");
  ctx.check(run_verdict("accept_play_sound_nil_optionals"),
            "play_sound: nil optionals accepted");
  ctx.check((g_recorder.playSoundCalls == 2U) &&
                (g_recorder.playSoundVolume == 1.0F) &&
                (g_recorder.playSoundPitch == 1.0F) &&
                g_recorder.playSoundLoop,
            "play_sound: nil optionals take the defaults, loop honoured");
  ctx.check(run_verdict("accept_play_sound_values"),
            "play_sound: finite values accepted");
  ctx.check((g_recorder.playSoundCalls == 3U) &&
                (g_recorder.playSoundVolume == 0.25F) &&
                (g_recorder.playSoundPitch == 1.5F) &&
                !g_recorder.playSoundLoop,
            "play_sound: finite values reach the service unchanged");

  ctx.check(run_verdict("reject_play_sound_at_position"),
            "play_sound_at: NaN position component returns false");
  ctx.check(run_verdict("reject_play_sound_at_volume"),
            "play_sound_at: infinite volume returns false");
  ctx.check(g_recorder.playSoundAtCalls == 0U,
            "play_sound_at: rejected arguments never reach the service");
  ctx.check(run_verdict("accept_play_sound_at_defaults"),
            "play_sound_at: omitted volume accepted");
  ctx.check((g_recorder.playSoundAtCalls == 1U) &&
                (g_recorder.playSoundAtX == 1.0F) &&
                (g_recorder.playSoundAtY == 2.0F) &&
                (g_recorder.playSoundAtZ == 3.0F) &&
                (g_recorder.playSoundAtVolume == 1.0F),
            "play_sound_at: omitted volume takes 1, position unchanged");
  ctx.check(run_verdict("accept_play_sound_at_volume"),
            "play_sound_at: finite volume accepted");
  ctx.check((g_recorder.playSoundAtCalls == 2U) &&
                (g_recorder.playSoundAtVolume == 0.75F),
            "play_sound_at: finite volume reaches the service");

  ctx.check(run_verdict("reject_bus_volume"),
            "set_bus_volume: non-finite volumes return without raising");
  ctx.check(g_recorder.setBusVolumeCalls == 0U,
            "set_bus_volume: rejected volumes never reach the service");
  ctx.check(run_verdict("accept_bus_volume"),
            "set_bus_volume: finite volume accepted");
  ctx.check((g_recorder.setBusVolumeCalls == 1U) &&
                (g_recorder.setBusVolumeBus == 2U) &&
                (g_recorder.setBusVolumeValue == 0.5F),
            "set_bus_volume: bus and volume reach the service");

  ctx.check(run_verdict("reject_play_music_volume"),
            "play_music: NaN volume returns false");
  ctx.check(g_recorder.playMusicCalls == 0U,
            "play_music: rejected volume never reaches the service");
  ctx.check(run_verdict("accept_play_music_defaults"),
            "play_music: omitted optionals accepted");
  ctx.check((g_recorder.playMusicCalls == 1U) &&
                (g_recorder.playMusicVolume == 1.0F) &&
                g_recorder.playMusicLoop,
            "play_music: omitted optionals take volume 1 and loop");
  ctx.check(run_verdict("accept_play_music_values"),
            "play_music: finite values accepted");
  ctx.check((g_recorder.playMusicCalls == 2U) &&
                (g_recorder.playMusicVolume == 0.25F) &&
                !g_recorder.playMusicLoop,
            "play_music: finite values reach the service unchanged");

  sc::shutdown_scripting();
  static_cast<void>(std::remove(kScriptPath));
  return ctx.finish("script_audio_finite_args");
}
