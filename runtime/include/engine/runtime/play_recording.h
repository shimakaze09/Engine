// Declares play recordings: a play session's input, recorded step by step,
// together with World::state_hash checkpoints, so a replay of the session
// shows whether it reproduced the recording and, where it did not, the
// first tick and hash section that differ. This is Unreal's
// demorec/demoplay with Doom's demo consistency check: a desync is
// reported, never silently replayed.
//
// A recording is two files beside each other:
//   <name>.input  the input log (engine/core/input.h), step for step
//   <name>.demo   the manifest: the input log's seal and the checkpoints
// The manifest is written last and names the log by its seal, so a log
// and a manifest that were not written together are refused rather than
// replayed. A recording never replaces an existing one: beginning one at a
// path that already holds a manifest is refused.
//
// Manifest revision 1 is text, one record per line, ending in a newline:
//   engine-play-recording 1
//   input <step count> <checksum, 16 hex>
//   checkpoints <count>
//   <tick> <entities> <transforms> <rigid bodies> <physics> <timers>
//          <animation> <random>          (one line; 16 hex each, ticks rising)
//   end
// The reader is strict: another revision, a malformed or missing line, a
// tick out of order, a count that disagrees or bytes after "end" refuse the
// whole document.
//
// The pipeline observes the world at the start and at the end of every
// frame, while a recording or replay is open. A recording keeps the hash
// at each tick it observes; a replay compares the hash at each tick both
// runs observed, so a replay at another frame schedule still meets the
// recording wherever their frames line up. The start of the first frame is
// tick 0, the scene as the session began, so a replay against a scene
// edited since the recording diverges at tick 0.

#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::runtime {

class World;

/// The one manifest revision this build reads and writes.
inline constexpr std::uint32_t kPlayRecordingVersion = 1U;

/// The most checkpoints one recording keeps. A recording that reaches it
/// halves its checkpoints -- keeping the ticks on a doubled stride, tick 0
/// always among them -- and records at that stride from then on, so a
/// long session stays checked from start to end, only more sparsely. At
/// one checkpoint a tick it holds about four and a half minutes at 60 Hz
/// before it thins.
inline constexpr std::size_t kMaxPlayCheckpoints = 16384U;

/// The manifest's extension; its log is the same path with ".input".
inline constexpr const char *kPlayRecordingExtension = ".demo";

/// The sections of World::state_hash, in the order the hash folds them.
/// A section's value is the fold so far, so the first section that
/// differs is where two runs' states first differ.
enum class PlayHashSection : std::uint8_t {
  Entities,
  Transforms,
  RigidBodies,
  Physics,
  Timers,
  Animation,
  Random,
  None,
};

/// The section's name as a log line prints it ("transforms").
const char *play_hash_section_name(PlayHashSection section) noexcept;

/// How the latest replay has gone so far.
struct PlayReplayReport final {
  /// Checkpoints the recording holds.
  std::uint64_t recorded = 0U;
  /// Checkpoints compared: ticks both runs observed.
  std::uint64_t compared = 0U;
  /// Of those, the ones whose hashes differ.
  std::uint64_t mismatched = 0U;
  /// The first compared tick that differs and its first differing section;
  /// None while every compared checkpoint matched.
  std::uint64_t firstDivergentTick = 0U;
  PlayHashSection firstDivergentSection = PlayHashSection::None;
  /// True once the replay passed the recording's last checkpoint or its
  /// session ended; the summary has been logged.
  bool finished = false;
};

/// Starts recording the play session: the input log at `demoPath` with
/// ".demo" replaced by ".input", and the checkpoints. Call it before the
/// session's first frame -- the editor does so while stopped, then starts
/// Play. False, logged, with nothing changed, when `demoPath` does not end
/// in ".demo" or does not fit, a manifest already exists there, a
/// recording or replay is open, or the input log cannot be staged.
bool begin_play_recording(const char *demoPath) noexcept;

/// Starts replaying the recording whose manifest is `demoPath`: the input
/// log replaces the live input step for step, and each checkpoint is
/// compared as the session reaches its tick. False, logged, with nothing
/// changed, when the manifest is missing, malformed or another revision,
/// its log is missing or damaged or is not the log it sealed, or a
/// recording or replay is open.
bool begin_play_replay(const char *demoPath) noexcept;

/// True while a recording is open.
bool play_recording_active() noexcept;
/// True while a replay is open, compared through or not.
bool play_replay_active() noexcept;
/// The open replay's report, or the last one's after it ended.
PlayReplayReport play_replay_report() noexcept;

/// Observes the world at `tickIndex`, the ticks simulated so far in this
/// session. The pipeline calls it at the start and end of every frame, on
/// the committed state (never during Simulation). Does nothing when no
/// recording or replay is open; a tick not past the last one observed is
/// ignored.
void observe_play_checkpoint(std::uint64_t tickIndex,
                             const World &world) noexcept;

/// Ends the open recording or replay, with the session whose ticks it is
/// keyed by. A recording seals its input log, then writes the manifest
/// through a staged atomic replace; a replay stops and logs its summary.
/// The pipeline calls it when a play session ends and at teardown. False
/// only when a recording could not be saved, which is logged; true
/// otherwise, nothing open included.
bool end_play_log() noexcept;

} // namespace engine::runtime
