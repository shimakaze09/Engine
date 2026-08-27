// Verifies the durable-replacement protocol behind every authored-file
// save (audit #338): the staged payload is flushed, synced and closed
// before the rename, the directory holding the new entry is synced
// after it, and each fault branch — a failed flush, sync, close or
// rename before the rename commits, a directory that cannot be opened
// or synced after it — reports the outcome its callers depend on. It
// also verifies the directory-creation companion (audit #357): each
// segment the engine creates on the way to a save has its parent synced
// before the next is made, so the commit's durability rests on a
// directory whose own entry reached storage. The filesystem operations
// are injected so those faults are exercised on the same functions
// core's commit path calls; the closing case of each half runs the
// production ops end to end.

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

using engine::core::detail::CreateDirectoryOutcome;
using engine::core::detail::DirectoryHandle;
using engine::core::detail::durable_create_directories;
using engine::core::detail::durable_replace;
using engine::core::detail::kDirectoryDurabilityUnavailable;
using engine::core::detail::kInvalidDirectoryHandle;
using engine::core::detail::MakeDirectoryOutcome;
using engine::core::detail::parent_directory_of;
using engine::core::detail::ReplaceOps;
using engine::core::detail::ReplaceOutcome;

/// The step a recording op stands for, so an assertion can name the
/// order the durability argument rests on.
enum class Step : int {
  MakeDirectory,
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
  /// The paths make_directory was asked to create, in order, so a case
  /// can assert which segments the walk visited.
  std::vector<std::string> createdPaths;
  /// What make_directory reports; a case that needs a specific segment
  /// to already exist names it in existingPath.
  MakeDirectoryOutcome makeResult = MakeDirectoryOutcome::Created;
  std::string existingPath;

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

/// Records the segment the walk asked for and reports the outcome the
/// case armed — AlreadyExists for the one segment it names as present.
MakeDirectoryOutcome fake_make_directory(const char *path) noexcept {
  static_cast<void>(g_recorder.run(Step::MakeDirectory));
  g_recorder.createdPaths.emplace_back((path != nullptr) ? path : "");
  if (!g_recorder.existingPath.empty() && (path != nullptr) &&
      (g_recorder.existingPath == path)) {
    return MakeDirectoryOutcome::AlreadyExists;
  }
  return g_recorder.makeResult;
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
  return ReplaceOps{&fake_make_directory,
                    &fake_flush,
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
/// payload lands exactly, no durability degradation is reported, and the
/// protocol's outcome is exactly the one this platform can deliver:
/// Durable where a directory-sync primitive exists, and the explicit
/// ReplacedDurabilityUnavailable on Windows until #358 lands.
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

  // `wrote && log.errors == 0` isolates Durable only where the platform
  // has a directory-sync primitive: ReplacedDurabilityUnavailable also
  // commits true and logs nothing, so on Windows the assertions above
  // would hold with no sync attempted at all. Assert the production
  // ops' exact outcome per platform, so the day Windows gains a real
  // primitive (issue #358) this case fails until it is updated.
  const std::string outcomeTemp =
      (directory / "outcome.json.staged").generic_string();
  const std::string outcomeDestination =
      (directory / "outcome.json").generic_string();
  std::FILE *staged = nullptr;
#ifdef _WIN32
  if (fopen_s(&staged, outcomeTemp.c_str(), "wb") != 0) {
    staged = nullptr;
  }
#else
  staged = std::fopen(outcomeTemp.c_str(), "wb");
#endif
  if (staged == nullptr) {
    ctx.fail("the production-ops case could not stage its file");
    std::filesystem::remove_all(directory, ec);
    return;
  }
  static_cast<void>(std::fputs(payload, staged));

  const ReplaceOutcome outcome =
      durable_replace(staged, outcomeTemp.c_str(), outcomeDestination.c_str(),
                      engine::core::detail::production_replace_ops());

#ifdef _WIN32
  ctx.check(outcome == ReplaceOutcome::ReplacedDurabilityUnavailable,
            "Windows reports the durability primitive as unavailable");
#else
  ctx.check(outcome == ReplaceOutcome::Durable,
            "the production ops sync the real parent directory");
#endif
  ctx.check(std::filesystem::exists(outcomeDestination, ec),
            "the production ops replaced the destination");

  std::filesystem::remove_all(directory, ec);
}

/// EXPECTATION (audit #357): creating the directory a save commits into
/// syncs the parent that now carries each new entry, and does it right
/// after the segment is created — so the whole path from an already
/// durable ancestor down to the destination is durable before any file
/// is written into it. On the base revision no directory-creation
/// protocol existed at all (callers issued a bare mkdir per segment and
/// synced nothing), so every DirectorySync step here is the regression
/// this case pins.
void check_directory_creation_ordering(engine::tests::TestContext &ctx) {
  begin_case();
  const ReplaceOps ops = recording_ops();

  const CreateDirectoryOutcome outcome =
      durable_create_directories("a/b/c", ops);

  ctx.check(outcome == CreateDirectoryOutcome::Durable,
            "a fully created path is durable");
  ctx.check(steps_are({Step::MakeDirectory, Step::DirectoryOpen,
                       Step::DirectorySync, Step::DirectoryClose,
                       Step::MakeDirectory, Step::DirectoryOpen,
                       Step::DirectorySync, Step::DirectoryClose,
                       Step::MakeDirectory, Step::DirectoryOpen,
                       Step::DirectorySync, Step::DirectoryClose}),
            "each created segment syncs its parent before the next is made");
  ctx.check(g_recorder.createdPaths ==
                std::vector<std::string>{"a", "a/b", "a/b/c"},
            "the walk creates each prefix in order, from the outermost in");
}

/// EXPECTATION: a path already present in full creates nothing and so
/// syncs nothing — the durability of an existing directory is not this
/// call's to establish, and paying for a sync per save would be a cost
/// with no entry to make durable.
void check_directory_creation_already_exists(engine::tests::TestContext &ctx) {
  begin_case();
  g_recorder.makeResult = MakeDirectoryOutcome::AlreadyExists;

  const CreateDirectoryOutcome outcome =
      durable_create_directories("a/b/c", recording_ops());

  ctx.check(outcome == CreateDirectoryOutcome::AlreadyExists,
            "an existing path reports that nothing was created");
  ctx.check(steps_are({Step::MakeDirectory, Step::MakeDirectory,
                       Step::MakeDirectory}),
            "an existing path issues no sync work");
}

/// EXPECTATION: only the newly created segment costs a sync when the
/// leading segments were already there — the common shape of a fresh
/// profile directory under an existing home or config root.
void check_directory_creation_partial(engine::tests::TestContext &ctx) {
  begin_case();
  g_recorder.existingPath = "a";

  const CreateDirectoryOutcome outcome =
      durable_create_directories("a/b", recording_ops());

  ctx.check(outcome == CreateDirectoryOutcome::Durable,
            "a partially created path is durable");
  ctx.check(steps_are({Step::MakeDirectory, Step::MakeDirectory,
                       Step::DirectoryOpen, Step::DirectorySync,
                       Step::DirectoryClose}),
            "the segment that already existed is not synced");
}

/// EXPECTATION: a parent that cannot be opened or synced is reported as
/// a created-but-not-durable path rather than swallowed. The directory
/// stands either way, so neither branch may report Failed — a caller
/// would take that as "no directory" and refuse a save that would in
/// fact succeed.
void check_directory_creation_sync_faults(engine::tests::TestContext &ctx) {
  {
    begin_case();
    g_recorder.openResult = kInvalidDirectoryHandle;
    const CreateDirectoryOutcome outcome =
        durable_create_directories("a/b", recording_ops());
    ctx.check(outcome == CreateDirectoryOutcome::CreatedNotDurable,
              "a parent that cannot be opened reports degraded durability");
    ctx.check(steps_are({Step::MakeDirectory, Step::DirectoryOpen,
                         Step::MakeDirectory, Step::DirectoryOpen}),
              "no sync is attempted on a parent that never opened, and the "
              "walk still finishes the path");
  }
  {
    begin_case();
    g_recorder.fail_at(Step::DirectorySync);
    const CreateDirectoryOutcome outcome =
        durable_create_directories("a/b", recording_ops());
    ctx.check(outcome == CreateDirectoryOutcome::CreatedNotDurable,
              "a failing parent sync reports degraded durability");
    ctx.check(g_recorder.createdPaths ==
                  std::vector<std::string>{"a", "a/b"},
              "a failed sync still creates the rest of the path");
  }
  {
    begin_case();
    g_recorder.openResult = kDirectoryDurabilityUnavailable;
    const CreateDirectoryOutcome outcome =
        durable_create_directories("a/b", recording_ops());
    ctx.check(outcome == CreateDirectoryOutcome::CreatedDurabilityUnavailable,
              "a platform with no directory-sync primitive says so");
    ctx.check(steps_are({Step::MakeDirectory, Step::DirectoryOpen,
                         Step::MakeDirectory, Step::DirectoryOpen}),
              "no sync or close is attempted where the primitive is absent");
  }
}

/// EXPECTATION: a segment that cannot be created fails the whole call
/// and stops the walk — the destination directory does not exist, so the
/// commit that would follow must not be attempted.
void check_directory_creation_failure(engine::tests::TestContext &ctx) {
  begin_case();
  g_recorder.makeResult = MakeDirectoryOutcome::Failed;

  const CreateDirectoryOutcome outcome =
      durable_create_directories("a/b/c", recording_ops());

  ctx.check(outcome == CreateDirectoryOutcome::Failed,
            "a segment that cannot be created fails the call");
  ctx.check(g_recorder.createdPaths == std::vector<std::string>{"a"},
            "the walk stops at the first failure instead of pressing on");
}

/// EXPECTATION: the shapes that name no creatable segment are stepped
/// over rather than handed to mkdir — an absolute path's leading
/// separator, a repeated or trailing separator, a Windows drive
/// designator — and a path with nothing to walk is refused outright.
void check_directory_creation_path_shapes(engine::tests::TestContext &ctx) {
  {
    begin_case();
    static_cast<void>(durable_create_directories("/a/b", recording_ops()));
    ctx.check(g_recorder.createdPaths ==
                  std::vector<std::string>{"/a", "/a/b"},
              "an absolute path's leading separator names no segment");
  }
  {
    begin_case();
    static_cast<void>(durable_create_directories("a//b/", recording_ops()));
    ctx.check(g_recorder.createdPaths ==
                  std::vector<std::string>{"a", "a//b"},
              "repeated and trailing separators create no empty segment");
  }
  {
    begin_case();
    static_cast<void>(durable_create_directories("C:/a", recording_ops()));
    ctx.check(g_recorder.createdPaths == std::vector<std::string>{"C:/a"},
              "a drive designator is stepped over, not created");
  }
  {
    begin_case();
    const CreateDirectoryOutcome rootOutcome =
        durable_create_directories("/", recording_ops());
    ctx.check(rootOutcome == CreateDirectoryOutcome::AlreadyExists,
              "the filesystem root has nothing to create");
    ctx.check(g_recorder.steps.empty(), "the root issues no filesystem step");
  }
  {
    begin_case();
    const ReplaceOps ops = recording_ops();
    const bool refused =
        (durable_create_directories(nullptr, ops) ==
         CreateDirectoryOutcome::Failed) &&
        (durable_create_directories("", ops) == CreateDirectoryOutcome::Failed);
    ctx.check(refused, "a null or empty path is refused");
    ctx.check(g_recorder.steps.empty(),
              "a refused path issues no filesystem step");
  }
  {
    begin_case();
    const std::string tooLong(1024U, 'x');
    ctx.check(durable_create_directories(tooLong.c_str(), recording_ops()) ==
                  CreateDirectoryOutcome::Failed,
              "a path that does not fit is refused, not truncated");
    ctx.check(g_recorder.steps.empty(),
              "an over-long path issues no filesystem step");
  }
}

/// EXPECTATION: the production ops create a real multi-segment path and
/// report exactly the durability this platform delivers, and a second
/// call over the same path reports that it created nothing. The name
/// taken by a plain file is the boundary that separates "already there"
/// from "cannot be created": mkdir reports EEXIST for both, and treating
/// the file as a directory would let a save be attempted into a path
/// that can never hold it.
void check_directory_creation_production(engine::tests::TestContext &ctx) {
  const std::filesystem::path root{"durable_create_test_dir"};
  std::error_code ec{};
  std::filesystem::remove_all(root, ec);

  const std::string nested = (root / "profile" / "saves").generic_string();
  const CreateDirectoryOutcome created = durable_create_directories(
      nested.c_str(), engine::core::detail::production_replace_ops());

#ifdef _WIN32
  ctx.check(created == CreateDirectoryOutcome::CreatedDurabilityUnavailable,
            "Windows reports the directory-sync primitive as unavailable");
#else
  ctx.check(created == CreateDirectoryOutcome::Durable,
            "the production ops sync each created directory's parent");
#endif
  ctx.check(std::filesystem::is_directory(nested, ec),
            "the production ops created the whole path");

  const CreateDirectoryOutcome again = durable_create_directories(
      nested.c_str(), engine::core::detail::production_replace_ops());
  ctx.check(again == CreateDirectoryOutcome::AlreadyExists,
            "a second call over the same path creates nothing");

  const std::string occupied = (root / "occupied").generic_string();
  const char *payload = "{}";
  const bool staged =
      engine::core::atomic_write_file(occupied.c_str(), payload, 2U);
  ctx.check(staged, "the occupied-name case staged its file");
  const std::string throughFile = (occupied + "/child");
  ctx.check(durable_create_directories(
                throughFile.c_str(),
                engine::core::detail::production_replace_ops()) ==
                CreateDirectoryOutcome::Failed,
            "a name held by a plain file fails instead of passing as a "
            "directory that already exists");

  std::filesystem::remove_all(root, ec);
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
  check_directory_creation_ordering(ctx);
  check_directory_creation_already_exists(ctx);
  check_directory_creation_partial(ctx);
  check_directory_creation_sync_faults(ctx);
  check_directory_creation_failure(ctx);
  check_directory_creation_path_shapes(ctx);
  check_directory_creation_production(ctx);

  static_cast<void>(std::fclose(g_stagedFile));
  g_stagedFile = nullptr;
  std::error_code stageEc{};
  std::filesystem::remove(kStagedFilePath, stageEc);

  return ctx.finish("durable_replace_test");
}
