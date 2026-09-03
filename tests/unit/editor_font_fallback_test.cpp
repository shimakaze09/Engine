// Verifies that initialize_editor survives a missing editor font file: the
// bundled face is loaded through a readable-file probe, so an absent asset
// takes the logged built-in-font fallback instead of tripping ImGui's hard
// assert inside its file loader (regression test for #408). Drives the
// production initialize_editor entry point from a working directory with
// no assets/ tree and reads the fallback through a registered log sink.

#include "editor_session.h"
#include "engine/core/logging.h"
#include "engine/editor/editor.h"
#include "../test_harness.h"

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace {

engine::tests::TestContext g_tests;

/// Counts the editor's font-fallback warning as the production log path
/// emits it; nothing else the editor logs during initialization matches.
struct FontWarningTally {
  std::size_t fallbackWarnings = 0U;
};

void tally_sink(engine::core::LogLevel level, const char *channel,
                const char *message, void *userData) noexcept {
  auto *tally = static_cast<FontWarningTally *>(userData);
  if ((level == engine::core::LogLevel::Warning) &&
      (std::strcmp(channel, "editor") == 0) &&
      (std::strstr(message, "editor font missing") != nullptr)) {
    ++tally->fallbackWarnings;
  }
}

/// Runs initialize_editor up to its forced-failure seam and returns how
/// many font-fallback warnings it logged on the way. The seam sits after
/// the font stage and before the ImGui platform backend, so no window
/// system is needed and a false return is the expected outcome; a
/// non-null stand-in window satisfies the identity checks SDL applies.
std::size_t font_warnings_during_initialize() noexcept {
  alignas(std::max_align_t) static char fakeWindow[512] = {};
  FontWarningTally tally{};
  if (!engine::core::log_register_sink(&tally_sink, &tally)) {
    g_tests.fail("log sink registered");
    return 0U;
  }
  engine::editor::editor_set_initialize_failure_for_tests(true);
  const bool initialized = engine::editor::initialize_editor(&fakeWindow[0]);
  engine::editor::editor_set_initialize_failure_for_tests(false);
  engine::core::log_unregister_sink(&tally_sink, &tally);
  g_tests.check(!initialized, "the seam stops initialization after the font");
  return tally.fallbackWarnings;
}

/// Moves the working directory to the nearest ancestor carrying the
/// bundled editor font (the pipeline tests' asset-walk technique).
bool enter_directory_with_editor_font() noexcept {
  std::error_code ec{};
  const std::filesystem::path original = std::filesystem::current_path(ec);
  if (ec) {
    return false;
  }
  const std::filesystem::path candidates[] = {
      original, original / "..", original / "../..", original / "../../..",
      original / "../../../.."};
  for (const std::filesystem::path &candidate : candidates) {
    const std::filesystem::path normalized =
        std::filesystem::weakly_canonical(candidate, ec);
    if (ec) {
      continue;
    }
    if (std::filesystem::exists(normalized / "assets/fonts/Roboto-Medium.ttf",
                                ec)) {
      std::filesystem::current_path(normalized, ec);
      return !ec;
    }
  }
  return false;
}

/// EXPECTATION (#408): from a working directory with no assets/ tree,
/// initialize_editor reaches its fallback branch, one "editor font
/// missing" warning through the production log path, instead of aborting
/// inside ImGui's font file loader. The unfixed revision never returns
/// from initialize_editor in assert-enabled builds.
void check_missing_font_takes_fallback() noexcept {
  std::error_code ec{};
  const std::filesystem::path original = std::filesystem::current_path(ec);
  if (ec) {
    g_tests.fail("current working directory is readable");
    return;
  }
  const std::filesystem::path scratch = original / "engine_font_fallback_test";
  std::filesystem::remove_all(scratch, ec);
  std::filesystem::create_directories(scratch, ec);
  if (ec) {
    g_tests.fail("scratch directory created");
    return;
  }
  std::filesystem::current_path(scratch, ec);
  if (ec) {
    g_tests.fail("scratch directory entered");
    return;
  }
  g_tests.check(
      !std::filesystem::exists("assets/fonts/Roboto-Medium.ttf", ec),
      "scratch working directory carries no editor font");

  g_tests.check(font_warnings_during_initialize() == 1U,
                "missing font: exactly one fallback warning, no abort");

  std::filesystem::current_path(original, ec);
  std::filesystem::remove_all(scratch, ec);
}

/// Control: with the bundled font present, the probe finds it and the
/// fallback is not taken, so the warning is specific to a missing file.
void check_present_font_is_loaded() noexcept {
  if (!enter_directory_with_editor_font()) {
    g_tests.fail("the bundled editor font could be located");
    return;
  }
  g_tests.check(font_warnings_during_initialize() == 0U,
                "present font: no fallback warning");
}

} // namespace

/// Runs this executable or test program.
int main() {
  g_tests.check(engine::core::initialize_logging(), "initialize_logging");
  check_missing_font_takes_fallback();
  check_present_font_is_loaded();
  engine::core::shutdown_logging();
  return g_tests.finish("editor_font_fallback_test");
}
