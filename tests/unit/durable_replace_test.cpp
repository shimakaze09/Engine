// Verifies the durable-replacement protocol behind every authored-file
// save (audit #338): the staged payload is flushed, synced and closed
// before the rename, the directory holding the new entry is synced
// after it, and each fault branch — a failed flush, sync, close or
// rename before the rename commits, a directory that cannot be opened
// or synced after it — reports the outcome its callers depend on. The
// filesystem operations are injected so those faults are exercised on
// the same function core's commit path calls; the closing case runs the
// production ops end to end through the public writer.

#include "../test_harness.h"

#include "durable_replace.h"

#include "engine/core/atomic_file.h"
#include "engine/core/logging.h"

#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

using engine::core::detail::DirectoryHandle;
using engine::core::detail::durable_replace;
using engine::core::detail::kDirectoryDurabilityUnavailable;
using engine::core::detail::kInvalidDirectoryHandle;
using engine::core::detail::parent_directory_of;
using engine::core::detail::ReplaceOps;
using engine::core::detail::ReplaceOutcome;

/// The step a recording op stands for, so an assertion can name the
/// order the durability argument rests on.
enum class Step : int {
  FileFlush,
  FileSync,
  FileClose,
  Rename,
  DirectoryOpen,
  DirectorySync,
  DirectoryClose,
  TemporaryRemove,
};

/// Records the steps the protocol issued and which one is set to fail.
/// A file-scope instance is the only way the C-linkage-style op
/// pointers in ReplaceOps can reach test state.
struct Recorder final {
  std::vector<Step> steps;
  Step failing = Step::TemporaryRemove;
  bool hasFailingStep = false;
  DirectoryHandle openResult = 7;
  bool renamed = false;
  bool temporaryRemoved = false;

  /// Records the step and reports whether it is the injected failure.
  bool run(Step step) {
    steps.push_back(step);
    return !(hasFailingStep && (step == failing));
  }

  /// Arms the injected failure for one step.
  void fail_at(Step step) {
    failing = step;
    hasFailingStep = true;
  }
};

Recorder g_recorder{};

/// Compares the recorded step sequence against the expected order.
bool steps_are(const std::vector<Step> &expected) {
  return g_recorder.steps == expected;
}

/// The staged file the fake ops receive and never dereference; a real
/// stream so the protocol is handed the same kind of handle production
/// gives it. Closed and removed once, by main — std::tmpfile is avoided
/// deliberately: the Windows CRT creates its temporary in the drive
/// root, which fails outright for a user without write access there.
constexpr const char *kStagedFilePath = "durable_replace_test_stage.tmp";
std::FILE *g_stagedFile = nullptr;

/// Opens the staged stand-in in the working directory.
std::FILE *open_staged_file() {
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, kStagedFilePath, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(kStagedFilePath, "wb");
#endif
  return file;
}

bool fake_flush(std::FILE *) noexcept { return g_recorder.run(Step::FileFlush); }
bool fake_sync(std::FILE *) noexcept { return g_recorder.run(Step::FileSync); }
bool fake_close(std::FILE *) noexcept { return g_recorder.run(Step::FileClose); }

/// Records the rename and reports the injected result.
bool fake_rename(const char *, const char *) noexcept {
  const bool ok = g_recorder.run(Step::Rename);
  g_recorder.renamed = ok;
  return ok;
}

/// Reports the handle the case under test arms, recording the attempt.
DirectoryHandle fake_open_directory(const char *) noexcept {
  static_cast<void>(g_recorder.run(Step::DirectoryOpen));
  return g_recorder.openResult;
}

bool fake_sync_directory(DirectoryHandle) noexcept {
  return g_recorder.run(Step::DirectorySync);
}

void fake_close_directory(DirectoryHandle) noexcept {
  static_cast<void>(g_recorder.run(Step::DirectoryClose));
}

/// Records that the staged temporary was discarded.
bool fake_remove(const char *) noexcept {
  g_recorder.temporaryRemoved = true;
  return g_recorder.run(Step::TemporaryRemove);
}

/// The recording table the injected-fault cases run through.
ReplaceOps recording_ops() noexcept {
  return ReplaceOps{&fake_flush,
                    &fake_sync,
                    &fake_close,
                    &fake_rename,
                    &fake_open_directory,
                    &fake_sync_directory,
                    &fake_close_directory,
                    &fake_remove};
}

/// Resets the recorder before a case and returns the staged file.
std::FILE *begin_case() {
  g_recorder = Recorder{};
  return g_stagedFile;
}

/// Counts the durability errors core's commit path logged.
struct DurabilityLog final {
  int errors = 0;
};

/// Sink counting only the durability report commit emits.
void count_durability_errors(engine::core::LogLevel level, const char *channel,
                             const char *message, void *userData) noexcept {
  static_cast<void>(message);
  auto *log = static_cast<DurabilityLog *>(userData);
  if ((log != nullptr) && (level == engine::core::LogLevel::Error) &&
      (channel != nullptr) && (std::strcmp(channel, "core.atomic_file") == 0)) {
    ++log->errors;
  }
}

/// EXPECTATION (audit #338): a replacement whose every step succeeds
/// syncs the directory holding the new entry, and does it after the
/// rename — the ordering the durability claim rests on. On the base
/// revision the protocol stopped at Rename, so the three directory
/// steps are the regression this case pins.
void check_durable_ordering(engine::tests::TestContext &ctx) {
  std::FILE *file = begin_case();
  const ReplaceOps ops = recording_ops();

  const ReplaceOutcome outcome =
      durable_replace(file, "stage.tmp", "dest.json", ops);

  ctx.check(outcome == ReplaceOutcome::Durable, "successful replace is durable");
  ctx.check(steps_are({Step::FileFlush, Step::FileSync, Step::FileClose,
                       Step::Rename, Step::DirectoryOpen, Step::DirectorySync,
                       Step::DirectoryClose}),
            "payload is synced before the rename and the directory after it");
  ctx.check(!g_recorder.temporaryRemoved,
            "a committed replacement removes no temporary");
}

/// EXPECTATION: a directory that cannot be opened after the rename
/// leaves the replacement standing — the destination already holds the
/// new payload — and reports the degraded durability instead of a
/// failure the caller would read as "the old file survived".
void check_directory_open_failure(engine::tests::TestContext &ctx) {
  std::FILE *file = begin_case();
  g_recorder.openResult = kInvalidDirectoryHandle;
  const ReplaceOps ops = recording_ops();

  const ReplaceOutcome outcome =
      durable_replace(file, "stage.tmp", "dest.json", ops);

  ctx.check(outcome == ReplaceOutcome::ReplacedNotDurable,
            "directory-open failure reports a replaced, non-durable write");
  ctx.check(g_recorder.renamed, "the rename still committed");
  ctx.check(steps_are({Step::FileFlush, Step::FileSync, Step::FileClose,
                       Step::Rename, Step::DirectoryOpen}),
            "no sync is attempted on a directory that never opened");
  ctx.check(!g_recorder.temporaryRemoved,
            "a committed replacement removes no temporary");
}

/// EXPECTATION: a failing directory sync reports the same degraded
/// outcome and still releases the directory handle.
void check_directory_sync_failure(engine::tests::TestContext &ctx) {
  std::FILE *file = begin_case();
  g_recorder.fail_at(Step::DirectorySync);
  const ReplaceOps ops = recording_ops();

  const ReplaceOutcome outcome =
      durable_replace(file, "stage.tmp", "dest.json", ops);

  ctx.check(outcome == ReplaceOutcome::ReplacedNotDurable,
            "directory-sync failure reports a replaced, non-durable write");
  ctx.check(steps_are({Step::FileFlush, Step::FileSync, Step::FileClose,
                       Step::Rename, Step::DirectoryOpen, Step::DirectorySync,
                       Step::DirectoryClose}),
            "a failed directory sync still closes the handle");
}

/// EXPECTATION: a platform with no directory-sync primitive says so
/// explicitly (Windows) instead of reporting a durability it never
/// established, and attempts neither a sync nor a close.
void check_directory_durability_unavailable(engine::tests::TestContext &ctx) {
  std::FILE *file = begin_case();
  g_recorder.openResult = kDirectoryDurabilityUnavailable;
  const ReplaceOps ops = recording_ops();

  const ReplaceOutcome outcome =
      durable_replace(file, "stage.tmp", "dest.json", ops);

  ctx.check(outcome == ReplaceOutcome::ReplacedDurabilityUnavailable,
            "an unavailable directory sync is reported as such");
  ctx.check(steps_are({Step::FileFlush, Step::FileSync, Step::FileClose,
                       Step::Rename, Step::DirectoryOpen}),
            "no directory sync or close is attempted");
}

/// EXPECTATION: every fault before the rename fails the replacement,
/// leaves the destination untouched, discards the temporary, and still
/// closes the staged file. A failed flush skips the sync it would have
/// forced; a failed sync and a failed close both stop short of the
/// rename.
void check_pre_rename_faults(engine::tests::TestContext &ctx) {
  {
    std::FILE *file = begin_case();
    g_recorder.fail_at(Step::FileFlush);
    const ReplaceOutcome outcome =
        durable_replace(file, "stage.tmp", "dest.json", recording_ops());
    ctx.check(outcome == ReplaceOutcome::Failed, "a failed flush fails");
    ctx.check(steps_are({Step::FileFlush, Step::FileClose,
                         Step::TemporaryRemove}),
              "a failed flush skips the sync, closes, and discards the stage");
    ctx.check(!g_recorder.renamed, "a failed flush never renames");
  }
  {
    std::FILE *file = begin_case();
    g_recorder.fail_at(Step::FileSync);
    const ReplaceOutcome outcome =
        durable_replace(file, "stage.tmp", "dest.json", recording_ops());
    ctx.check(outcome == ReplaceOutcome::Failed, "a failed sync fails");
    ctx.check(steps_are({Step::FileFlush, Step::FileSync, Step::FileClose,
                         Step::TemporaryRemove}),
              "a failed sync closes and discards the stage without renaming");
  }
  {
    std::FILE *file = begin_case();
    g_recorder.fail_at(Step::FileClose);
    const ReplaceOutcome outcome =
        durable_replace(file, "stage.tmp", "dest.json", recording_ops());
    ctx.check(outcome == ReplaceOutcome::Failed, "a failed close fails");
    ctx.check(g_recorder.temporaryRemoved,
              "a failed close discards the stage");
    ctx.check(!g_recorder.renamed, "a failed close never renames");
  }
  {
    std::FILE *file = begin_case();
    g_recorder.fail_at(Step::Rename);
    const ReplaceOutcome outcome =
        durable_replace(file, "stage.tmp", "dest.json", recording_ops());
    ctx.check(outcome == ReplaceOutcome::Failed, "a failed rename fails");
    ctx.check(steps_are({Step::FileFlush, Step::FileSync, Step::FileClose,
                         Step::Rename, Step::TemporaryRemove}),
              "a failed rename discards the stage and touches no directory");
  }
}

/// EXPECTATION: a null staged file or path fails before any filesystem
/// operation runs, so a misuse cannot delete or rename anything.
void check_null_arguments(engine::tests::TestContext &ctx) {
  std::FILE *file = begin_case();
  const ReplaceOps ops = recording_ops();

  const bool allFailed =
      (durable_replace(nullptr, "stage.tmp", "dest.json", ops) ==
       ReplaceOutcome::Failed) &&
      (durable_replace(file, nullptr, "dest.json", ops) ==
       ReplaceOutcome::Failed) &&
      (durable_replace(file, "stage.tmp", nullptr, ops) ==
       ReplaceOutcome::Failed);

  ctx.check(allFailed, "null arguments fail the replacement");
  ctx.check(g_recorder.steps.empty(), "null arguments issue no filesystem step");
}

/// EXPECTATION: the directory whose entry gets synced is the one that
/// holds the destination, including for a bare name (the working
/// directory) and a root-level path.
void check_parent_directory_resolution(engine::tests::TestContext &ctx) {
  char buffer[64] = {};

  ctx.check(parent_directory_of(buffer, sizeof(buffer), "assets/scene.json") &&
                (std::strcmp(buffer, "assets") == 0),
            "a relative path resolves to its directory");
  ctx.check(parent_directory_of(buffer, sizeof(buffer), "a/b/c.json") &&
                (std::strcmp(buffer, "a/b") == 0),
            "a nested path keeps every leading component");
  ctx.check(parent_directory_of(buffer, sizeof(buffer), "scene.json") &&
                (std::strcmp(buffer, ".") == 0),
            "a bare name resolves to the working directory");
  ctx.check(parent_directory_of(buffer, sizeof(buffer), "/scene.json") &&
                (std::strcmp(buffer, "/") == 0),
            "a root-level path resolves to the root directory");

  char tiny[3] = {};
  ctx.check(!parent_directory_of(tiny, sizeof(tiny), "assets/scene.json") &&
                (tiny[0] == '\0'),
            "a result that does not fit is refused, not truncated");
  ctx.check(!parent_directory_of(buffer, sizeof(buffer), nullptr) &&
                (buffer[0] == '\0'),
            "a null path is refused");
  ctx.check(!parent_directory_of(nullptr, sizeof(buffer), "a/b"),
            "a null destination is refused");
}

/// EXPECTATION: the production ops run the whole protocol against a real
/// filesystem — the public writer commits a file in a subdirectory, the
/// payload lands exactly, and no durability degradation is reported,
/// which is only true when the real parent-directory sync succeeded.
void check_production_path(engine::tests::TestContext &ctx) {
  const std::filesystem::path directory{"durable_replace_test_dir"};
  std::error_code ec{};
  std::filesystem::remove_all(directory, ec);
  if (!std::filesystem::create_directory(directory, ec) || ec) {
    ctx.fail("production case could not stage its directory");
    return;
  }

  const std::string destination =
      (directory / "authored.json").generic_string();
  const char *payload = "{\"durable\":true}";

  DurabilityLog log{};
  const bool loggingReady = engine::core::initialize_logging();
  const bool sinkReady =
      loggingReady && engine::core::log_register_sink(&count_durability_errors,
                                                      &log);

  const bool wrote = engine::core::atomic_write_file(destination.c_str(),
                                                     payload,
                                                     std::strlen(payload));

  if (sinkReady) {
    engine::core::log_unregister_sink(&count_durability_errors, &log);
  }
  if (loggingReady) {
    engine::core::shutdown_logging();
  }

  ctx.check(wrote, "the production writer commits through the protocol");
  ctx.check(std::filesystem::exists(destination, ec),
            "the destination exists after the commit");
  ctx.check(std::filesystem::file_size(destination, ec) ==
                std::strlen(payload),
            "the destination holds exactly the payload");
  ctx.check(sinkReady, "the durability sink was registered");
  ctx.check(log.errors == 0,
            "the real parent-directory sync reported no degradation");

  std::filesystem::remove_all(directory, ec);
}

} // namespace

/// Runs this executable or test program.
int main() {
  engine::tests::TestContext ctx{};

  g_stagedFile = open_staged_file();
  if (g_stagedFile == nullptr) {
    ctx.fail("the staged file stand-in could not be opened");
    return ctx.finish("durable_replace_test");
  }

  check_durable_ordering(ctx);
  check_directory_open_failure(ctx);
  check_directory_sync_failure(ctx);
  check_directory_durability_unavailable(ctx);
  check_pre_rename_faults(ctx);
  check_null_arguments(ctx);
  check_parent_directory_resolution(ctx);
  check_production_path(ctx);

  static_cast<void>(std::fclose(g_stagedFile));
  g_stagedFile = nullptr;
  std::error_code stageEc{};
  std::filesystem::remove(kStagedFilePath, stageEc);

  return ctx.finish("durable_replace_test");
}
