// Verifies shader hot-reload swap semantics for the Engine test suite
// (audit H-09, reworked for #296's cooked-only loading): a successful
// reload replaces the device program id behind the same handle,
// destroys the old device program, and advances the reload epoch that
// program-id/uniform caches key their refresh on; a failed reload keeps
// the old program and does not signal. The fake device consumes cooked
// binaries through create_program_binary — the production entry point —
// and the watched files are the cooked binaries themselves, so a recook
// (mtime change) triggers the reload.

#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/renderer/render_device.h"
#include "engine/renderer/shader_system.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace {

constexpr const char *kCookedDir = "bgfx/cooked";
constexpr const char *kVertBinFile =
    "bgfx/cooked/_shader_reload_test.vert.default.spirv.bin";
constexpr const char *kFragBinFile =
    "bgfx/cooked/_shader_reload_test.frag.default.spirv.bin";
constexpr const char *kVertVfsPath = "shdr/_shader_reload_test.vert";
constexpr const char *kFragVfsPath = "shdr/_shader_reload_test.frag";

engine::renderer::RenderDevice g_fakeDevice{};
std::uint32_t g_nextProgram = 100U;
std::uint32_t g_lastDestroyedProgram = 0U;
std::uint32_t g_destroyedProgramCount = 0U;
std::uint32_t g_linkAttemptCount = 0U;
std::uint32_t g_shaderLogCount = 0U;

/// Counts "shader"-channel diagnostics so the retry-storm regression can
/// assert the per-frame poll stays silent for an unchanged bad generation.
void counting_log_sink(engine::core::LogLevel /*level*/, const char *channel,
                       const char * /*message*/, void * /*userData*/) noexcept {
  if ((channel != nullptr) && (std::strcmp(channel, "shader") == 0)) {
    ++g_shaderLogCount;
  }
}

const char *cooked_profile() noexcept { return "spirv"; }

/// Links when neither cooked blob carries the FAILME sentinel — the
/// fake stand-in for a corrupt/unlinkable binary.
engine::renderer::DeviceProgramHandle
fake_create_program_binary(const void *vertData, std::ptrdiff_t vertSize,
                           const void *fragData,
                           std::ptrdiff_t fragSize) noexcept {
  const auto broken = [](const void *data, std::ptrdiff_t size) noexcept {
    if (data == nullptr) {
      return true;
    }
    const std::string_view text(static_cast<const char *>(data),
                                static_cast<std::size_t>(size));
    return text.find("FAILME") != std::string_view::npos;
  };
  ++g_linkAttemptCount;
  if (broken(vertData, vertSize) || broken(fragData, fragSize)) {
    return engine::renderer::kInvalidDeviceProgram;
  }
  return engine::renderer::DeviceProgramHandle{++g_nextProgram};
}

void fake_destroy_program(
    engine::renderer::DeviceProgramHandle program) noexcept {
  g_lastDestroyedProgram = program.value;
  ++g_destroyedProgramCount;
}

void configure_fake_device() noexcept {
  g_fakeDevice = engine::renderer::RenderDevice{};
  g_fakeDevice.caps.cookedPrograms = true;
  g_fakeDevice.cooked_program_profile = &cooked_profile;
  g_fakeDevice.create_program_binary = &fake_create_program_binary;
  g_fakeDevice.destroy_program = &fake_destroy_program;
}

bool write_file(const char *path, const char *text) noexcept {
  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "wb") != 0) {
    return false;
  }
#else
  file = std::fopen(path, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t len = std::strlen(text);
  const bool ok = std::fwrite(text, 1U, len, file) == len;
  std::fclose(file);
  return ok;
}

/// Advances the file's recorded mtime by one whole second so the reload
/// poll observes a change without any wall-clock sleep.
bool bump_mtime(const char *path) noexcept {
  std::error_code ec{};
  const auto current = std::filesystem::last_write_time(path, ec);
  if (ec) {
    return false;
  }
  std::filesystem::last_write_time(path, current + std::chrono::seconds(1),
                                   ec);
  return !ec;
}

void remove_files() noexcept {
  static_cast<void>(std::remove(kVertBinFile));
  static_cast<void>(std::remove(kFragBinFile));
}

/// EXPECTATION (audit H-09): the initial load and every successful hot
/// reload advance shader_reload_epoch and swap the id behind the same
/// handle while destroying the old device program; a failed relink
/// keeps the old program, does not destroy it, and does not move the
/// epoch.
int check_reload_swaps_program_and_signals_epoch() {
  configure_fake_device();
  remove_files();
  std::error_code ec{};
  std::filesystem::create_directories(kCookedDir, ec);
  if (ec) {
    return 217;
  }

  if (!write_file(kVertBinFile, "cooked-vert-v1") ||
      !write_file(kFragBinFile, "cooked-frag-v1")) {
    return 201;
  }

  if (!engine::renderer::initialize_shader_system()) {
    return 202;
  }

  const std::uint64_t epochBeforeLoad = engine::renderer::shader_reload_epoch();
  const engine::renderer::ShaderProgramHandle handle =
      engine::renderer::load_shader_program(kVertVfsPath, kFragVfsPath);
  if (handle == engine::renderer::kInvalidShaderProgram) {
    return 203;
  }
  const engine::renderer::DeviceProgramHandle firstProgram =
      engine::renderer::shader_device_program(handle);
  if (firstProgram == engine::renderer::kInvalidDeviceProgram) {
    return 204;
  }
  if (engine::renderer::shader_reload_epoch() != epochBeforeLoad + 1U) {
    return 205;
  }

  // A recook rewrites the cooked binary; the watcher must relink.
  if (!write_file(kFragBinFile, "cooked-frag-v2") ||
      !bump_mtime(kFragBinFile)) {
    return 206;
  }
  g_destroyedProgramCount = 0U;
  engine::renderer::check_shader_reload();

  const engine::renderer::DeviceProgramHandle secondProgram =
      engine::renderer::shader_device_program(handle);
  if (secondProgram == firstProgram) {
    return 207;
  }
  if (secondProgram == engine::renderer::kInvalidDeviceProgram) {
    return 208;
  }
  if ((g_destroyedProgramCount != 1U) ||
      (g_lastDestroyedProgram != firstProgram.value)) {
    return 209;
  }
  if (engine::renderer::shader_reload_epoch() != epochBeforeLoad + 2U) {
    return 210;
  }

  // A corrupt recook must not replace or destroy the live program.
  if (!write_file(kFragBinFile, "FAILME") || !bump_mtime(kFragBinFile)) {
    return 211;
  }
  g_destroyedProgramCount = 0U;
  engine::renderer::check_shader_reload();

  if (engine::renderer::shader_device_program(handle) != secondProgram) {
    return 212;
  }
  if (g_destroyedProgramCount != 0U) {
    return 213;
  }
  if (engine::renderer::shader_reload_epoch() != epochBeforeLoad + 2U) {
    return 214;
  }

  g_destroyedProgramCount = 0U;
  engine::renderer::destroy_shader_program(handle);
  if ((g_destroyedProgramCount != 1U) ||
      (g_lastDestroyedProgram != secondProgram.value)) {
    return 215;
  }
  if (engine::renderer::shader_device_program(handle) !=
      engine::renderer::kInvalidDeviceProgram) {
    return 216;
  }

  engine::renderer::shutdown_shader_system();
  remove_files();
  return 0;
}

/// EXPECTATION (#391): a failed hot reload is attempted (and diagnosed)
/// exactly once per watched-input generation — the per-frame poll must not
/// repeat file reads, link attempts, or error logs for content already
/// known bad. Each new generation (vertex-only, fragment-only, or both)
/// earns exactly one new attempt, and a good recook recovers with one
/// attempt after which the poll goes quiet again.
int check_failed_reload_attempts_once_per_generation() {
  configure_fake_device();
  remove_files();
  std::error_code ec{};
  std::filesystem::create_directories(kCookedDir, ec);
  if (ec) {
    return 30;
  }
  if (!write_file(kVertBinFile, "cooked-vert-v1") ||
      !write_file(kFragBinFile, "cooked-frag-v1")) {
    return 31;
  }
  if (!engine::renderer::initialize_shader_system()) {
    return 32;
  }
  if (!engine::core::log_register_sink(&counting_log_sink, nullptr)) {
    return 33;
  }

  const engine::renderer::ShaderProgramHandle handle =
      engine::renderer::load_shader_program(kVertVfsPath, kFragVfsPath);
  if (handle == engine::renderer::kInvalidShaderProgram) {
    return 34;
  }
  const engine::renderer::DeviceProgramHandle loaded =
      engine::renderer::shader_device_program(handle);
  const std::uint64_t epochLoaded = engine::renderer::shader_reload_epoch();

  // One corrupt fragment generation: exactly one attempt however many
  // frames poll it, and the diagnostics stop growing after the first poll.
  if (!write_file(kFragBinFile, "FAILME") || !bump_mtime(kFragBinFile)) {
    return 35;
  }
  g_linkAttemptCount = 0U;
  engine::renderer::check_shader_reload();
  const std::uint32_t logsAfterFirstPoll = g_shaderLogCount;
  engine::renderer::check_shader_reload();
  engine::renderer::check_shader_reload();
  if (g_linkAttemptCount != 1U) {
    return 36;
  }
  if (g_shaderLogCount != logsAfterFirstPoll) {
    return 37;
  }
  if (engine::renderer::shader_device_program(handle) != loaded) {
    return 38;
  }
  if (engine::renderer::shader_reload_epoch() != epochLoaded) {
    return 39;
  }

  // The same bad content touched again is a new generation: one attempt.
  if (!bump_mtime(kFragBinFile)) {
    return 40;
  }
  g_linkAttemptCount = 0U;
  engine::renderer::check_shader_reload();
  engine::renderer::check_shader_reload();
  if (g_linkAttemptCount != 1U) {
    return 41;
  }

  // Vertex-only generation: one attempt.
  if (!write_file(kVertBinFile, "FAILME") || !bump_mtime(kVertBinFile)) {
    return 42;
  }
  g_linkAttemptCount = 0U;
  engine::renderer::check_shader_reload();
  engine::renderer::check_shader_reload();
  if (g_linkAttemptCount != 1U) {
    return 43;
  }

  // Both stages changed in one generation: still one attempt.
  if (!bump_mtime(kVertBinFile) || !bump_mtime(kFragBinFile)) {
    return 44;
  }
  g_linkAttemptCount = 0U;
  engine::renderer::check_shader_reload();
  engine::renderer::check_shader_reload();
  if (g_linkAttemptCount != 1U) {
    return 45;
  }

  // Recovery: a good recook relinks on one attempt, then the poll goes
  // quiet — no further attempts for the now-current generation.
  if (!write_file(kVertBinFile, "cooked-vert-v2") ||
      !write_file(kFragBinFile, "cooked-frag-v2") ||
      !bump_mtime(kVertBinFile) || !bump_mtime(kFragBinFile)) {
    return 46;
  }
  g_linkAttemptCount = 0U;
  engine::renderer::check_shader_reload();
  const engine::renderer::DeviceProgramHandle recovered =
      engine::renderer::shader_device_program(handle);
  engine::renderer::check_shader_reload();
  engine::renderer::check_shader_reload();
  if (g_linkAttemptCount != 1U) {
    return 47;
  }
  if ((recovered == loaded) ||
      (recovered == engine::renderer::kInvalidDeviceProgram)) {
    return 48;
  }
  if (engine::renderer::shader_device_program(handle) != recovered) {
    return 49;
  }
  if (engine::renderer::shader_reload_epoch() != epochLoaded + 1U) {
    return 50;
  }

  engine::renderer::destroy_shader_program(handle);
  engine::core::log_unregister_sink(&counting_log_sink, nullptr);
  engine::renderer::shutdown_shader_system();
  remove_files();
  return 0;
}

} // namespace

namespace engine::renderer {

bool initialize_render_device() noexcept { return true; }

void shutdown_render_device() noexcept {}

const RenderDevice *render_device() noexcept { return &g_fakeDevice; }

} // namespace engine::renderer

/// Runs this executable or test program.
int main() {
  if (!engine::core::initialize_logging()) {
    return 200;
  }
  if (!engine::core::initialize_vfs()) {
    engine::core::shutdown_logging();
    return 199;
  }
  if (!engine::core::mount("shdr", ".")) {
    engine::core::shutdown_vfs();
    engine::core::shutdown_logging();
    return 198;
  }

  int result = check_reload_swaps_program_and_signals_epoch();
  if (result == 0) {
    result = check_failed_reload_attempts_once_per_generation();
  }

  engine::core::shutdown_vfs();
  engine::core::shutdown_logging();
  return result;
}
