// Implements the input log: records what each fixed step read into a
// versioned binary document, and replays it into the steps in place of the
// recorded events, so a run reproduces step for step at any frame schedule.
//
// Layout of revision 1, every integer little-endian:
//   header  "ENGINPUT", u32 version
//   steps   u64 tick, u8 flags, [previous frame when flags bit 0], frame
//   footer  "INPUTEND", u64 step count, u64 FNV-1a 64 of every byte before
//           the checksum
// A frame is the InputFrame's fields in declaration order. The previous
// frame is written only where it is not the step before's frame -- the
// first step, and a step after a reset -- so an ordinary step costs one.
// A replay validates the whole log before it changes anything, then keeps
// the bytes and decodes one step at a time, so no run is too long to
// record and the only limit on replaying one is its bytes in memory.

#include "engine/core/atomic_file.h"
#include "engine/core/fixed_ring.h"
#include "engine/core/hash.h"
#include "engine/core/input.h"
#include "engine/core/logging.h"
#include "engine/core/nothrow_buffer.h"
#include "input_frame.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

namespace engine::core {

namespace {

constexpr std::array<std::uint8_t, 8U> kHeaderMagic = {'E', 'N', 'G', 'I',
                                                       'N', 'P', 'U', 'T'};
constexpr std::array<std::uint8_t, 8U> kFooterMagic = {'I', 'N', 'P', 'U',
                                                       'T', 'E', 'N', 'D'};
constexpr std::size_t kHeaderBytes = 8U + 4U;
constexpr std::size_t kFooterBytes = 8U + 8U + 8U;
constexpr std::size_t kGamepads = static_cast<std::size_t>(kMaxGamepads);
constexpr std::size_t kFrameBytes =
    (3U * kKeyWords * 8U) + (5U * 4U) + 3U + (2U * kGamepads * 2U) +
    (kGamepads * static_cast<std::size_t>(kMaxGamepadAxes) * 2U);
constexpr std::size_t kMaxStepBytes = 8U + 1U + (2U * kFrameBytes);

/// The step carries its own previous frame.
constexpr std::uint8_t kFlagPrevious = 1U;

// ----- Encoding --------------------------------------------------------------

/// Appends little-endian integers to a fixed buffer the caller sized.
struct ByteWriter final {
  std::uint8_t *data = nullptr;
  std::size_t size = 0U;

  void u8(std::uint8_t value) noexcept { data[size++] = value; }
  void u16(std::uint16_t value) noexcept {
    u8(static_cast<std::uint8_t>(value & 0xFFU));
    u8(static_cast<std::uint8_t>(value >> 8U));
  }
  void u32(std::uint32_t value) noexcept {
    u16(static_cast<std::uint16_t>(value & 0xFFFFU));
    u16(static_cast<std::uint16_t>(value >> 16U));
  }
  void u64(std::uint64_t value) noexcept {
    u32(static_cast<std::uint32_t>(value & 0xFFFFFFFFU));
    u32(static_cast<std::uint32_t>(value >> 32U));
  }
  void i16(std::int16_t value) noexcept {
    u16(static_cast<std::uint16_t>(value));
  }
  void i32(int value) noexcept { u32(static_cast<std::uint32_t>(value)); }
  void bytes(const std::uint8_t *source, std::size_t count) noexcept {
    std::memcpy(data + size, source, count);
    size += count;
  }
};

/// Reads little-endian integers; a read past the end fails and stays failed.
struct ByteReader final {
  const std::uint8_t *data = nullptr;
  std::size_t size = 0U;
  std::size_t pos = 0U;
  bool ok = true;

  std::uint8_t u8() noexcept {
    if (!ok || (pos >= size)) {
      ok = false;
      return 0U;
    }
    return data[pos++];
  }
  std::uint16_t u16() noexcept {
    const std::uint16_t low = u8();
    const std::uint16_t high = u8();
    return static_cast<std::uint16_t>(low | (high << 8U));
  }
  std::uint32_t u32() noexcept {
    const std::uint32_t low = u16();
    const std::uint32_t high = u16();
    return low | (high << 16U);
  }
  std::uint64_t u64() noexcept {
    const std::uint64_t low = u32();
    const std::uint64_t high = u32();
    return low | (high << 32U);
  }
  std::int16_t i16() noexcept { return static_cast<std::int16_t>(u16()); }
  int i32() noexcept { return static_cast<int>(u32()); }
  bool magic(const std::array<std::uint8_t, 8U> &expected) noexcept {
    bool same = true;
    for (const std::uint8_t byte : expected) {
      same = (u8() == byte) && same;
    }
    return ok && same;
  }
};

void write_keys(ByteWriter &out,
                const std::array<std::uint64_t, kKeyWords> &keys) noexcept {
  for (const std::uint64_t word : keys) {
    out.u64(word);
  }
}

void write_frame(ByteWriter &out, const InputFrame &frame) noexcept {
  write_keys(out, frame.keyDown);
  write_keys(out, frame.keyPressed);
  write_keys(out, frame.keyReleased);
  out.i32(frame.mouseX);
  out.i32(frame.mouseY);
  out.i32(frame.mouseDeltaX);
  out.i32(frame.mouseDeltaY);
  out.i32(frame.scrollDelta);
  out.u8(frame.mouseDown);
  out.u8(frame.mousePressed);
  out.u8(frame.gamepadConnected);
  for (const std::uint16_t down : frame.gamepadDown) {
    out.u16(down);
  }
  for (const std::uint16_t pressed : frame.gamepadPressed) {
    out.u16(pressed);
  }
  for (const auto &axes : frame.gamepadAxes) {
    for (const std::int16_t axis : axes) {
      out.i16(axis);
    }
  }
}

void read_keys(ByteReader &in,
               std::array<std::uint64_t, kKeyWords> &keys) noexcept {
  for (std::uint64_t &word : keys) {
    word = in.u64();
  }
}

/// Bits a frame cannot set: keys past the keyboard page, mouse buttons and
/// controller slots past the tracked ones. Any of them set is damage.
bool frame_in_domain(const InputFrame &frame) noexcept {
  constexpr std::size_t kUsedBits = static_cast<std::size_t>(kMaxScancodes);
  constexpr std::uint64_t kLastWordMask =
      ((kUsedBits % 64U) == 0U)
          ? ~std::uint64_t{0}
          : ((std::uint64_t{1} << (kUsedBits % 64U)) - 1U);
  const std::size_t last = kKeyWords - 1U;
  const bool keysOk = ((frame.keyDown[last] & ~kLastWordMask) == 0U) &&
                      ((frame.keyPressed[last] & ~kLastWordMask) == 0U) &&
                      ((frame.keyReleased[last] & ~kLastWordMask) == 0U);
  constexpr unsigned kMouseMask = (1U << kMaxMouseButtons) - 1U;
  constexpr unsigned kPadMask = (1U << kMaxGamepads) - 1U;
  return keysOk && ((frame.mouseDown & ~kMouseMask) == 0U) &&
         ((frame.mousePressed & ~kMouseMask) == 0U) &&
         ((frame.gamepadConnected & ~kPadMask) == 0U);
}

bool read_frame(ByteReader &in, InputFrame &frame) noexcept {
  read_keys(in, frame.keyDown);
  read_keys(in, frame.keyPressed);
  read_keys(in, frame.keyReleased);
  frame.mouseX = in.i32();
  frame.mouseY = in.i32();
  frame.mouseDeltaX = in.i32();
  frame.mouseDeltaY = in.i32();
  frame.scrollDelta = in.i32();
  frame.mouseDown = in.u8();
  frame.mousePressed = in.u8();
  frame.gamepadConnected = in.u8();
  for (std::uint16_t &down : frame.gamepadDown) {
    down = in.u16();
  }
  for (std::uint16_t &pressed : frame.gamepadPressed) {
    pressed = in.u16();
  }
  for (auto &axes : frame.gamepadAxes) {
    for (std::int16_t &axis : axes) {
      axis = in.i16();
    }
  }
  return in.ok && frame_in_domain(frame);
}

std::uint64_t checksum_append(std::uint64_t hash, const std::uint8_t *data,
                              std::size_t size) noexcept {
  for (std::size_t i = 0U; i < size; ++i) {
    hash = fnv1a_64_append(hash, data[i]);
  }
  return hash;
}

/// Decodes the step after `prior` (the step before it, or nothing for the
/// first) into `out`. Returns the reason the step is refused, or nullptr.
const char *read_step(ByteReader &in, const InputStepRecord *prior,
                      InputStepRecord &out) noexcept {
  out.tick = in.u64();
  const std::uint8_t flags = in.u8();
  if (!in.ok) {
    return "a step is cut short";
  }
  if ((flags & ~kFlagPrevious) != 0U) {
    return "a step carries an unknown flag";
  }
  const bool withPrevious = (flags & kFlagPrevious) != 0U;
  if (prior == nullptr) {
    if (!withPrevious) {
      return "the first step lacks its previous frame";
    }
  } else if (out.tick != (prior->tick + 1U)) {
    return "step ticks are not consecutive";
  }
  if (withPrevious) {
    if (!read_frame(in, out.previous)) {
      return "a step's previous frame is malformed or cut short";
    }
  } else {
    out.previous = prior->current;
  }
  if (!read_frame(in, out.current)) {
    return "a step's frame is malformed or cut short";
  }
  return nullptr;
}

// ----- Recording -------------------------------------------------------------

/// The open recording. The ring holds the steps since the last drain; a
/// drain encodes them into the staged temporary, which replaces the
/// destination only when the recording ends.
struct Recorder final {
  AtomicFileWriter writer{};
  FixedRing<InputStepRecord, kInputLogRingCapacity> ring{};
  // The frame of the step written last, which the next step's previous
  // frame usually is; meaningful once wroteStep is set.
  InputFrame lastWritten{};
  std::uint64_t checksum = kFnv1a64Offset;
  std::uint64_t stepCount = 0U;
  std::uint64_t nextTick = 0U;
  bool wroteStep = false;
  bool open = false;
  bool failed = false;
};

Recorder g_recorder{};

/// Writes `size` bytes to the staged file and folds them into the checksum.
bool recorder_write(const std::uint8_t *data, std::size_t size) noexcept {
  if (!g_recorder.writer.write(data, size)) {
    return false;
  }
  g_recorder.checksum = checksum_append(g_recorder.checksum, data, size);
  return true;
}

/// Stops the recording on a failure; the staged file is discarded and the
/// destination keeps whatever it held.
void recorder_fail(const char *reason) noexcept {
  g_recorder.failed = true;
  g_recorder.writer.abort();
  g_recorder.ring.clear();
  log_message(LogLevel::Error, "input", reason);
}

/// Encodes every step in the ring into the staged file.
bool recorder_drain() noexcept {
  std::array<std::uint8_t, kMaxStepBytes> buffer{};
  InputStepRecord step{};
  while (g_recorder.ring.pop(&step)) {
    const bool withPrevious =
        !g_recorder.wroteStep || !(step.previous == g_recorder.lastWritten);
    ByteWriter out{buffer.data(), 0U};
    out.u64(step.tick);
    out.u8(withPrevious ? kFlagPrevious : std::uint8_t{0U});
    if (withPrevious) {
      write_frame(out, step.previous);
    }
    write_frame(out, step.current);
    if (!recorder_write(buffer.data(), out.size)) {
      recorder_fail("input recording stopped: a write to the staged log "
                    "failed; the destination is unchanged");
      return false;
    }
    g_recorder.lastWritten = step.current;
    g_recorder.wroteStep = true;
  }
  return true;
}

/// Closes the recording state; a staged file still open is discarded.
void recorder_reset() noexcept {
  g_recorder.writer.abort();
  g_recorder.ring.clear();
  g_recorder.lastWritten = InputFrame{};
  g_recorder.checksum = kFnv1a64Offset;
  g_recorder.stepCount = 0U;
  g_recorder.nextTick = 0U;
  g_recorder.wroteStep = false;
  g_recorder.open = false;
  g_recorder.failed = false;
}

// ----- Replay ----------------------------------------------------------------

/// The loaded log, validated whole when it loaded, and where the next step
/// starts in it. Each step is decoded as it is taken, so a replay holds the
/// log's bytes and one step, however long the run.
struct Replay final {
  NothrowBuffer<std::uint8_t> bytes{};
  std::size_t offset = 0U;
  std::uint64_t remaining = 0U;
  // The step taken last: the one the pointer handed out names, and the
  // prior the next step decodes against.
  InputStepRecord step{};
  bool active = false;
};

Replay g_replay{};

void log_refusal(const char *path, const char *reason) noexcept {
  char message[640] = {};
  std::snprintf(message, sizeof(message),
                "input replay refused for '%s': %s; nothing changed", path,
                reason);
  log_message(LogLevel::Error, "input", message);
}

/// Validates a whole log and reports its step count and first tick. On
/// failure the reason names what is wrong.
const char *validate_log(const std::uint8_t *data, std::size_t size,
                         std::uint64_t *outStepCount,
                         std::uint64_t *outFirstTick) noexcept {
  if (size < (kHeaderBytes + kFooterBytes)) {
    return "the file is shorter than an empty log (truncated)";
  }
  ByteReader header{data, size, 0U, true};
  if (!header.magic(kHeaderMagic)) {
    return "not an input log (no ENGINPUT header)";
  }
  // The revision gate comes before any layout check: another revision's
  // layout is not this one's, so it is refused for its version alone.
  if (header.u32() != kInputLogVersion) {
    return "unsupported log version (this build reads version 1 only)";
  }
  ByteReader footer{data, size, size - kFooterBytes, true};
  if (!footer.magic(kFooterMagic)) {
    return "no end marker: the log is truncated or was never sealed";
  }
  const std::uint64_t stepCount = footer.u64();
  const std::size_t checksumAt = footer.pos;
  if (footer.u64() != checksum_append(kFnv1a64Offset, data, checksumAt)) {
    return "checksum mismatch: the log is damaged";
  }
  ByteReader body{data, size - kFooterBytes, kHeaderBytes, true};
  InputStepRecord prior{};
  InputStepRecord step{};
  for (std::uint64_t i = 0U; i < stepCount; ++i) {
    if (const char *reason =
            read_step(body, (i == 0U) ? nullptr : &prior, step);
        reason != nullptr) {
      return reason;
    }
    if (i == 0U) {
      *outFirstTick = step.tick;
    }
    prior = step;
  }
  if (body.pos != body.size) {
    return "the steps do not fill the log exactly";
  }
  *outStepCount = stepCount;
  return nullptr;
}

/// Reads the whole file into `outBytes`; the reason on failure, else null.
const char *read_log_file(const char *path,
                          NothrowBuffer<std::uint8_t> &outBytes) noexcept {
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "rb");
#endif
  if (file == nullptr) {
    return "the file cannot be opened";
  }
  // 64-bit offsets: a long run's log can pass what a long holds on Windows.
#ifdef _WIN32
  bool ok = (_fseeki64(file, 0, SEEK_END) == 0);
  const std::int64_t length = ok ? _ftelli64(file) : -1;
  ok = ok && (length >= 0) && (_fseeki64(file, 0, SEEK_SET) == 0);
#else
  bool ok = (fseeko(file, 0, SEEK_END) == 0);
  const std::int64_t length = ok ? static_cast<std::int64_t>(ftello(file)) : -1;
  ok = ok && (length >= 0) && (fseeko(file, 0, SEEK_SET) == 0);
#endif
  const char *reason = nullptr;
  if (!ok) {
    reason = "the file's size cannot be read";
  } else if (static_cast<std::uint64_t>(length) >
             static_cast<std::uint64_t>(
                 std::numeric_limits<std::size_t>::max())) {
    reason = "the file is larger than this process can address";
  } else if (!outBytes.allocate(static_cast<std::size_t>(length))) {
    reason = "out of memory for the log";
  } else if ((length > 0) && (std::fread(outBytes.data(), 1U, outBytes.size(),
                                         file) != outBytes.size())) {
    reason = "the file cannot be read";
  }
  static_cast<void>(std::fclose(file));
  return reason;
}

} // namespace

// ----- Recording API ---------------------------------------------------------

bool begin_input_recording(const char *path) noexcept {
  if (g_recorder.open) {
    log_message(LogLevel::Warning, "input",
                "input recording refused: one is already open");
    return false;
  }
  if ((path == nullptr) || (path[0] == '\0')) {
    log_message(LogLevel::Warning, "input",
                "input recording refused: no destination path");
    return false;
  }
  recorder_reset();
  if (!g_recorder.writer.begin(path)) {
    log_message(LogLevel::Error, "input",
                "input recording refused: the staged log cannot be created");
    return false;
  }
  std::array<std::uint8_t, kHeaderBytes> header{};
  ByteWriter out{header.data(), 0U};
  out.bytes(kHeaderMagic.data(), kHeaderMagic.size());
  out.u32(kInputLogVersion);
  if (!recorder_write(header.data(), out.size)) {
    recorder_reset();
    log_message(LogLevel::Error, "input",
                "input recording refused: the log header cannot be written");
    return false;
  }
  g_recorder.open = true;
  return true;
}

bool end_input_recording() noexcept {
  if (!g_recorder.open) {
    return false;
  }
  bool ok = !g_recorder.failed && recorder_drain();
  if (ok) {
    std::array<std::uint8_t, kFooterBytes> footer{};
    ByteWriter out{footer.data(), 0U};
    out.bytes(kFooterMagic.data(), kFooterMagic.size());
    out.u64(g_recorder.stepCount);
    // The checksum covers every byte before it, the count included.
    ok = recorder_write(footer.data(), out.size);
    const std::size_t checksumAt = out.size;
    out.u64(g_recorder.checksum);
    ok = ok &&
         g_recorder.writer.write(footer.data() + checksumAt,
                                 out.size - checksumAt) &&
         g_recorder.writer.commit();
    if (!ok) {
      log_message(LogLevel::Error, "input",
                  "input recording could not be sealed; the destination is "
                  "unchanged");
    }
  }
  recorder_reset();
  return ok;
}

bool input_recording_active() noexcept {
  return g_recorder.open && !g_recorder.failed;
}

void input_log_record_step(const InputStepRecord &step) noexcept {
  if (!g_recorder.open || g_recorder.failed) {
    return;
  }
  if ((g_recorder.stepCount > 0U) && (step.tick != g_recorder.nextTick)) {
    recorder_fail("input recording stopped: the steps skipped a tick, so the "
                  "log could not replay them");
    return;
  }
  if (g_recorder.ring.full() && !recorder_drain()) {
    return;
  }
  static_cast<void>(g_recorder.ring.push(step));
  ++g_recorder.stepCount;
  g_recorder.nextTick = step.tick + 1U;
}

// ----- Replay API ------------------------------------------------------------

bool begin_input_replay(const char *path) noexcept {
  if ((path == nullptr) || (path[0] == '\0')) {
    log_message(LogLevel::Error, "input",
                "input replay refused: no log path; nothing changed");
    return false;
  }
  NothrowBuffer<std::uint8_t> staged{};
  std::uint64_t stepCount = 0U;
  std::uint64_t firstTick = 0U;
  const char *reason = read_log_file(path, staged);
  if (reason == nullptr) {
    reason = validate_log(staged.data(), staged.size(), &stepCount, &firstTick);
  }
  if (reason != nullptr) {
    log_refusal(path, reason);
    return false;
  }
  g_replay.bytes = std::move(staged);
  g_replay.offset = kHeaderBytes;
  g_replay.remaining = stepCount;
  g_replay.step = InputStepRecord{};
  g_replay.active = (stepCount > 0U);
  char message[640] = {};
  std::snprintf(message, sizeof(message),
                "input replay of '%s': %llu steps from tick %llu", path,
                static_cast<unsigned long long>(stepCount),
                static_cast<unsigned long long>(firstTick));
  log_message(LogLevel::Info, "input", message);
  return true;
}

void end_input_replay() noexcept {
  g_replay.bytes.clear();
  g_replay.offset = 0U;
  g_replay.remaining = 0U;
  g_replay.step = InputStepRecord{};
  g_replay.active = false;
}

bool input_replay_active() noexcept { return g_replay.active; }

const InputStepRecord *input_log_take_replay_step(std::uint64_t tick) noexcept {
  if (!g_replay.active) {
    return nullptr;
  }
  const bool first = (g_replay.offset == kHeaderBytes);
  ByteReader in{g_replay.bytes.data(), g_replay.bytes.size() - kFooterBytes,
                g_replay.offset, true};
  InputStepRecord next{};
  // The log was validated whole when it loaded, so the decode cannot fail.
  static_cast<void>(read_step(in, first ? nullptr : &g_replay.step, next));
  if (next.tick != tick) {
    char message[256] = {};
    std::snprintf(message, sizeof(message),
                  "input replay stopped: step %llu is not the log's next "
                  "step %llu; the steps read the live devices again",
                  static_cast<unsigned long long>(tick),
                  static_cast<unsigned long long>(next.tick));
    log_message(LogLevel::Error, "input", message);
    g_replay.active = false;
    return nullptr;
  }
  g_replay.step = next;
  g_replay.offset = in.pos;
  if (--g_replay.remaining == 0U) {
    g_replay.active = false;
    log_message(LogLevel::Info, "input", "input replay finished");
  }
  return &g_replay.step;
}

void input_log_shutdown() noexcept {
  recorder_reset();
  end_input_replay();
}

} // namespace engine::core
