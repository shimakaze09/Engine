// Implements play recordings: the checkpoints kept beside the input log,
// the manifest that binds the two, and the replay's comparison and report.

#include "engine/runtime/play_recording.h"

#include "engine/core/atomic_file.h"
#include "engine/core/file_read.h"
#include "engine/core/input.h"
#include "engine/core/logging.h"
#include "engine/core/nothrow_buffer.h"
#include "engine/runtime/world.h"

#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <utility>

namespace engine::runtime {

namespace {

constexpr std::size_t kSectionCount = 7U;
constexpr std::size_t kMaxPathBytes = 1024U;
constexpr const char *kInputExtension = ".input";
constexpr const char *kMagicLine = "engine-play-recording";
/// The widest checkpoint line: a 20-digit tick and seven 16-hex hashes,
/// each after a space, and the newline.
constexpr std::size_t kMaxCheckpointLine = 20U + (kSectionCount * 17U) + 1U;
/// The widest manifest: its fixed lines and every checkpoint line.
constexpr std::size_t kMaxManifestBytes =
    256U + (kMaxPlayCheckpoints * kMaxCheckpointLine);

struct Checkpoint final {
  std::uint64_t tick = 0U;
  std::array<std::uint64_t, kSectionCount> sections{};
};

std::array<std::uint64_t, kSectionCount>
sections_of(const StateHashSections &hash) noexcept {
  return {hash.entities, hash.transforms, hash.rigidBodies, hash.physics,
          hash.timers,   hash.animation,  hash.random};
}

enum class Mode : std::uint8_t { Idle, Recording, Replaying };

/// The open recording or replay. The checkpoint storage is allocated when
/// one begins, so observing a frame never allocates.
struct PlayLog final {
  Mode mode = Mode::Idle;
  char demoPath[kMaxPathBytes] = {};
  core::NothrowBuffer<Checkpoint> checkpoints{};
  std::size_t count = 0U;
  // A recording keeps the ticks that are multiples of this; it doubles
  // each time the storage fills.
  std::uint64_t stride = 1U;
  bool observed = false;
  std::uint64_t lastTick = 0U;
  // A replay's next recorded checkpoint not yet passed.
  std::size_t cursor = 0U;
  PlayReplayReport report{};
};

PlayLog g_log{};

/// Logs "<lead> '<path>'<middle><detail>".
void log_path(core::LogLevel level, const char *lead, const char *path,
              const char *middle, const char *detail) noexcept {
  char message[kMaxPathBytes + 320U] = {};
  std::snprintf(message, sizeof(message), "%s '%s'%s%s", lead, path, middle,
                detail);
  core::log_message(level, "replay", message);
}

void reset_log() noexcept {
  g_log.mode = Mode::Idle;
  g_log.demoPath[0] = '\0';
  g_log.checkpoints.clear();
  g_log.count = 0U;
  g_log.stride = 1U;
  g_log.observed = false;
  g_log.lastTick = 0U;
  g_log.cursor = 0U;
}

/// Writes `demoPath` with ".demo" replaced by ".input" into `out`. False
/// when the path does not end in ".demo" or either path does not fit.
bool input_path_for(const char *demoPath, char *out,
                    std::size_t capacity) noexcept {
  if (demoPath == nullptr) {
    return false;
  }
  const std::size_t length = std::strlen(demoPath);
  const std::size_t extension = std::strlen(kPlayRecordingExtension);
  if ((length <= extension) || (length >= kMaxPathBytes) ||
      (std::strcmp(demoPath + (length - extension), kPlayRecordingExtension) !=
       0)) {
    return false;
  }
  const int written = std::snprintf(out, capacity, "%.*s%s",
                                    static_cast<int>(length - extension),
                                    demoPath, kInputExtension);
  return (written > 0) && (static_cast<std::size_t>(written) < capacity);
}

bool file_exists(const char *path) noexcept {
  char probe[1] = {};
  return core::read_whole_file(path, probe, sizeof(probe), nullptr) !=
         core::FileReadResult::Absent;
}

// ----- Manifest encoding
// ------------------------------------------------------

/// Appends lines to a fixed text buffer; a line that does not fit, or
/// that its formatting cut short, fails the whole document.
struct TextWriter final {
  char *data = nullptr;
  std::size_t capacity = 0U;
  std::size_t size = 0U;
  bool ok = true;

  void append(const char *line, int formatted) noexcept {
    const std::size_t length = std::strlen(line);
    ok = ok && (formatted > 0) &&
         (static_cast<std::size_t>(formatted) == length) &&
         (length < (capacity - size));
    if (ok) {
      std::memcpy(data + size, line, length);
      size += length;
    }
  }
};

bool write_manifest(const char *path, const core::InputLogSeal &seal) noexcept {
  core::NothrowBuffer<char> text{};
  if (!text.allocate(256U + (g_log.count * kMaxCheckpointLine))) {
    log_path(core::LogLevel::Error, "play recording", path,
             " was not saved: ", "out of memory for its manifest");
    return false;
  }
  TextWriter out{text.data(), text.size(), 0U, true};
  char line[kMaxCheckpointLine + 32U] = {};
  out.append(line, std::snprintf(line, sizeof(line), "%s %" PRIu32 "\n",
                                 kMagicLine, kPlayRecordingVersion));
  out.append(line, std::snprintf(line, sizeof(line),
                                 "input %" PRIu64 " %016" PRIx64 "\n",
                                 seal.stepCount, seal.checksum));
  out.append(line, std::snprintf(line, sizeof(line), "checkpoints %zu\n",
                                 g_log.count));
  for (std::size_t i = 0U; i < g_log.count; ++i) {
    const Checkpoint &point = g_log.checkpoints[i];
    out.append(line, std::snprintf(line, sizeof(line),
                                   "%" PRIu64 " %016" PRIx64 " %016" PRIx64
                                   " %016" PRIx64 " %016" PRIx64 " %016" PRIx64
                                   " %016" PRIx64 " %016" PRIx64 "\n",
                                   point.tick, point.sections[0],
                                   point.sections[1], point.sections[2],
                                   point.sections[3], point.sections[4],
                                   point.sections[5], point.sections[6]));
  }
  out.append("end\n", 4);
  if (!out.ok || !core::atomic_write_file(path, text.data(), out.size)) {
    log_path(core::LogLevel::Error, "play recording", path, " was not saved: ",
             "its manifest could not be written; any earlier file there is "
             "unchanged");
    return false;
  }
  return true;
}

// ----- Manifest decoding
// ------------------------------------------------------

/// Reads a manifest line by line, strictly: each token must be exactly
/// what the writer produces.
struct TextReader final {
  const char *at = nullptr;
  const char *end = nullptr;

  bool literal(const char *text) noexcept {
    const std::size_t length = std::strlen(text);
    if ((static_cast<std::size_t>(end - at) < length) ||
        (std::memcmp(at, text, length) != 0)) {
      return false;
    }
    at += length;
    return true;
  }
  /// A decimal without sign or leading zero (bar "0" itself).
  bool decimal(std::uint64_t *out) noexcept {
    const char *start = at;
    std::uint64_t value = 0U;
    while ((at < end) && (*at >= '0') && (*at <= '9')) {
      const auto digit = static_cast<std::uint64_t>(*at - '0');
      if (value > ((UINT64_MAX - digit) / 10U)) {
        return false;
      }
      value = (value * 10U) + digit;
      ++at;
    }
    const std::size_t digits = static_cast<std::size_t>(at - start);
    if ((digits == 0U) || ((digits > 1U) && (*start == '0'))) {
      return false;
    }
    *out = value;
    return true;
  }
  /// Exactly sixteen lowercase hex digits.
  bool hex16(std::uint64_t *out) noexcept {
    if ((end - at) < 16) {
      return false;
    }
    std::uint64_t value = 0U;
    for (int i = 0; i < 16; ++i, ++at) {
      const char c = *at;
      std::uint64_t digit = 0U;
      if ((c >= '0') && (c <= '9')) {
        digit = static_cast<std::uint64_t>(c - '0');
      } else if ((c >= 'a') && (c <= 'f')) {
        digit = static_cast<std::uint64_t>(c - 'a' + 10);
      } else {
        return false;
      }
      value = (value << 4U) | digit;
    }
    *out = value;
    return true;
  }
};

/// Parses a whole manifest into the replay state. The reason on failure,
/// else null.
const char *parse_manifest(const char *text, std::size_t size,
                           core::InputLogSeal *outSeal) noexcept {
  TextReader in{text, text + size};
  std::uint64_t version = 0U;
  if (!in.literal(kMagicLine) || !in.literal(" ") || !in.decimal(&version) ||
      !in.literal("\n")) {
    return "not a play recording (no engine-play-recording line)";
  }
  if (version != kPlayRecordingVersion) {
    return "unsupported recording version (this build reads version 1 only)";
  }
  core::InputLogSeal seal{};
  std::uint64_t count = 0U;
  if (!in.literal("input ") || !in.decimal(&seal.stepCount) ||
      !in.literal(" ") || !in.hex16(&seal.checksum) || !in.literal("\n")) {
    return "the input line is malformed";
  }
  if (!in.literal("checkpoints ") || !in.decimal(&count) || !in.literal("\n")) {
    return "the checkpoints line is malformed";
  }
  if (count > kMaxPlayCheckpoints) {
    return "more checkpoints than a recording holds";
  }
  if (!g_log.checkpoints.allocate(
          (count == 0U) ? 1U : static_cast<std::size_t>(count))) {
    return "out of memory for the checkpoints";
  }
  for (std::uint64_t i = 0U; i < count; ++i) {
    Checkpoint point{};
    if (!in.decimal(&point.tick)) {
      return "a checkpoint line is malformed or missing";
    }
    for (std::uint64_t &section : point.sections) {
      if (!in.literal(" ") || !in.hex16(&section)) {
        return "a checkpoint line is malformed or missing";
      }
    }
    if (!in.literal("\n")) {
      return "a checkpoint line is malformed or missing";
    }
    if ((i > 0U) && (point.tick <= g_log.checkpoints[i - 1U].tick)) {
      return "the checkpoint ticks do not rise";
    }
    g_log.checkpoints[static_cast<std::size_t>(i)] = point;
  }
  if (!in.literal("end\n")) {
    return "no end line: the manifest is truncated";
  }
  if (in.at != in.end) {
    return "bytes follow the end line";
  }
  g_log.count = static_cast<std::size_t>(count);
  *outSeal = seal;
  return nullptr;
}

// ----- Observation
// ------------------------------------------------------------

void record_checkpoint(
    std::uint64_t tick,
    const std::array<std::uint64_t, kSectionCount> &sections) noexcept {
  if ((tick % g_log.stride) != 0U) {
    return;
  }
  while (g_log.count == g_log.checkpoints.size()) {
    // Full: keep the ticks on a doubled stride, tick 0 among them, and
    // record at that stride from here.
    g_log.stride *= 2U;
    std::size_t kept = 0U;
    for (std::size_t i = 0U; i < g_log.count; ++i) {
      if ((g_log.checkpoints[i].tick % g_log.stride) == 0U) {
        g_log.checkpoints[kept] = g_log.checkpoints[i];
        ++kept;
      }
    }
    g_log.count = kept;
    if ((tick % g_log.stride) != 0U) {
      return;
    }
  }
  g_log.checkpoints[g_log.count] = Checkpoint{tick, sections};
  ++g_log.count;
}

void log_replay_summary() noexcept {
  PlayReplayReport &report = g_log.report;
  report.finished = true;
  char detail[256] = {};
  if (report.compared == 0U) {
    log_path(core::LogLevel::Warning, "replay of", g_log.demoPath,
             " checked nothing: ",
             "the session reached no tick the recording observed");
  } else if (report.mismatched == 0U) {
    std::snprintf(detail, sizeof(detail),
                  "all %" PRIu64 " checkpoints compared (of %" PRIu64
                  " recorded)",
                  report.compared, report.recorded);
    log_path(core::LogLevel::Info, "replay of", g_log.demoPath,
             " matched the recording at ", detail);
  } else {
    std::snprintf(detail, sizeof(detail),
                  "%" PRIu64 " of %" PRIu64
                  " checkpoints differ, the first at tick %" PRIu64 " in %s",
                  report.mismatched, report.compared, report.firstDivergentTick,
                  play_hash_section_name(report.firstDivergentSection));
    log_path(core::LogLevel::Warning, "replay of", g_log.demoPath,
             " diverged: ", detail);
  }
}

void compare_checkpoint(
    std::uint64_t tick,
    const std::array<std::uint64_t, kSectionCount> &sections) noexcept {
  PlayReplayReport &report = g_log.report;
  if (report.finished) {
    return;
  }
  while ((g_log.cursor < g_log.count) &&
         (g_log.checkpoints[g_log.cursor].tick < tick)) {
    ++g_log.cursor;
  }
  if ((g_log.cursor < g_log.count) &&
      (g_log.checkpoints[g_log.cursor].tick == tick)) {
    const Checkpoint &recorded = g_log.checkpoints[g_log.cursor];
    ++g_log.cursor;
    ++report.compared;
    std::size_t differs = kSectionCount;
    for (std::size_t i = 0U; i < kSectionCount; ++i) {
      if (recorded.sections[i] != sections[i]) {
        differs = i;
        break;
      }
    }
    if (differs != kSectionCount) {
      ++report.mismatched;
      if (report.firstDivergentSection == PlayHashSection::None) {
        report.firstDivergentTick = tick;
        report.firstDivergentSection = static_cast<PlayHashSection>(differs);
        char detail[128] = {};
        std::snprintf(detail, sizeof(detail), "tick %" PRIu64 " in %s", tick,
                      play_hash_section_name(report.firstDivergentSection));
        log_path(core::LogLevel::Warning, "replay of", g_log.demoPath,
                 " diverged from the recording at ", detail);
      }
    }
  }
  if (g_log.cursor == g_log.count) {
    log_replay_summary();
  }
}

} // namespace

const char *play_hash_section_name(PlayHashSection section) noexcept {
  switch (section) {
  case PlayHashSection::Entities:
    return "entities";
  case PlayHashSection::Transforms:
    return "transforms";
  case PlayHashSection::RigidBodies:
    return "rigid bodies";
  case PlayHashSection::Physics:
    return "physics";
  case PlayHashSection::Timers:
    return "timers";
  case PlayHashSection::Animation:
    return "animation";
  case PlayHashSection::Random:
    return "random";
  case PlayHashSection::None:
    break;
  }
  return "none";
}

bool begin_play_recording(const char *demoPath) noexcept {
  char inputPath[kMaxPathBytes] = {};
  const char *path = (demoPath != nullptr) ? demoPath : "";
  if (g_log.mode != Mode::Idle) {
    log_path(core::LogLevel::Warning, "play recording", path,
             " refused: ", "a recording or replay is already open");
    return false;
  }
  if (!input_path_for(demoPath, inputPath, sizeof(inputPath))) {
    log_path(core::LogLevel::Error, "play recording", path,
             " refused: ", "the path must end in .demo and fit");
    return false;
  }
  if (file_exists(demoPath)) {
    log_path(core::LogLevel::Error, "play recording", path, " refused: ",
             "a recording already exists there, and one is never replaced");
    return false;
  }
  if (!g_log.checkpoints.allocate(kMaxPlayCheckpoints)) {
    log_path(core::LogLevel::Error, "play recording", path,
             " refused: ", "out of memory for its checkpoints");
    return false;
  }
  if (!core::begin_input_recording(inputPath)) {
    reset_log();
    log_path(core::LogLevel::Error, "play recording", path,
             " refused: ", "its input log could not be started");
    return false;
  }
  g_log.mode = Mode::Recording;
  std::snprintf(g_log.demoPath, sizeof(g_log.demoPath), "%s", demoPath);
  log_path(core::LogLevel::Info, "recording play to", path, "", "");
  return true;
}

bool begin_play_replay(const char *demoPath) noexcept {
  const char *path = (demoPath != nullptr) ? demoPath : "";
  if (g_log.mode != Mode::Idle) {
    log_path(core::LogLevel::Warning, "replay of", path,
             " refused: ", "a recording or replay is already open");
    return false;
  }
  char inputPath[kMaxPathBytes] = {};
  if (!input_path_for(demoPath, inputPath, sizeof(inputPath))) {
    log_path(core::LogLevel::Error, "replay of", path,
             " refused: ", "the path must end in .demo and fit");
    return false;
  }
  core::NothrowBuffer<char> text{};
  if (!text.allocate(kMaxManifestBytes + 1U)) {
    log_path(core::LogLevel::Error, "replay of", path,
             " refused: ", "out of memory for its manifest");
    return false;
  }
  std::size_t size = 0U;
  const core::FileReadResult read =
      core::read_whole_file(demoPath, text.data(), text.size(), &size);
  if (read != core::FileReadResult::Ok) {
    log_path(core::LogLevel::Error, "replay of", path, " refused: ",
             (read == core::FileReadResult::Absent)
                 ? "no recording there"
                 : ((read == core::FileReadResult::TooLarge)
                        ? "the manifest is larger than a recording can be"
                        : "the manifest cannot be read"));
    return false;
  }
  core::InputLogSeal expected{};
  if (const char *reason = parse_manifest(text.data(), size, &expected);
      reason != nullptr) {
    reset_log();
    log_path(core::LogLevel::Error, "replay of", path, " refused: ", reason);
    return false;
  }
  core::InputLogSeal loaded{};
  if (!core::begin_input_replay(inputPath, &loaded)) {
    reset_log();
    log_path(core::LogLevel::Error, "replay of", path,
             " refused: ", "its input log is missing or damaged");
    return false;
  }
  if (!(loaded == expected)) {
    core::end_input_replay();
    reset_log();
    log_path(core::LogLevel::Error, "replay of", path,
             " refused: ", "its input log is not the one the recording sealed");
    return false;
  }
  g_log.mode = Mode::Replaying;
  std::snprintf(g_log.demoPath, sizeof(g_log.demoPath), "%s", demoPath);
  g_log.report = PlayReplayReport{};
  g_log.report.recorded = g_log.count;
  char detail[128] = {};
  std::snprintf(detail, sizeof(detail), ": %" PRIu64 " steps, %zu checkpoints",
                loaded.stepCount, g_log.count);
  log_path(core::LogLevel::Info, "replaying", path, "", detail);
  return true;
}

bool play_recording_active() noexcept { return g_log.mode == Mode::Recording; }

bool play_replay_active() noexcept { return g_log.mode == Mode::Replaying; }

PlayReplayReport play_replay_report() noexcept { return g_log.report; }

void observe_play_checkpoint(std::uint64_t tickIndex,
                             const World &world) noexcept {
  if (g_log.mode == Mode::Idle) {
    return;
  }
  if (g_log.observed && (tickIndex <= g_log.lastTick)) {
    return;
  }
  g_log.observed = true;
  g_log.lastTick = tickIndex;
  StateHashSections hash{};
  static_cast<void>(world.state_hash(&hash));
  if (g_log.mode == Mode::Recording) {
    record_checkpoint(tickIndex, sections_of(hash));
  } else {
    compare_checkpoint(tickIndex, sections_of(hash));
  }
}

bool end_play_log() noexcept {
  bool ok = true;
  if (g_log.mode == Mode::Recording) {
    core::InputLogSeal seal{};
    if (!core::end_input_recording(&seal)) {
      log_path(core::LogLevel::Error, "play recording", g_log.demoPath,
               " was not saved: ", "its input log could not be sealed");
      ok = false;
    } else if (write_manifest(g_log.demoPath, seal)) {
      char detail[128] = {};
      std::snprintf(detail, sizeof(detail),
                    " (%" PRIu64 " steps, %zu checkpoints)", seal.stepCount,
                    g_log.count);
      log_path(core::LogLevel::Info, "play recording saved to", g_log.demoPath,
               "", detail);
    } else {
      ok = false;
    }
  } else if (g_log.mode == Mode::Replaying) {
    core::end_input_replay();
    if (!g_log.report.finished) {
      log_replay_summary();
    }
  }
  reset_log();
  return ok;
}

} // namespace engine::runtime
