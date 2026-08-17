// Verifies shader hot-reload swap semantics for the Engine test suite
// (audit H-09): a successful reload replaces the GPU program id behind
// the same handle, destroys the old GL program, and advances the reload
// epoch that program-id/uniform caches key their refresh on; a failed
// reload keeps the old program and does not signal.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/renderer/render_device.h"
#include "engine/renderer/shader_system.h"

namespace {

constexpr const char *kVertFile = "_shader_reload_test.vert";
constexpr const char *kFragFile = "_shader_reload_test.frag";
constexpr const char *kVertVfsPath = "shdr/_shader_reload_test.vert";
constexpr const char *kFragVfsPath = "shdr/_shader_reload_test.frag";

engine::renderer::RenderDevice g_fakeDevice{};
std::uint32_t g_nextProgram = 100U;
std::uint32_t g_lastDestroyedProgram = 0U;
std::uint32_t g_destroyedProgramCount = 0U;
bool g_failNextCompiles = false;

engine::renderer::DeviceProgramHandle
fake_create_program(const char *vertSource, const char *fragSource) noexcept {
  const auto broken = [](const char *source) noexcept {
    return (source != nullptr) && (std::strstr(source, "FAILME") != nullptr);
  };
  if (g_failNextCompiles || broken(vertSource) || broken(fragSource)) {
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
  g_fakeDevice.create_program = &fake_create_program;
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
  static_cast<void>(std::remove(kVertFile));
  static_cast<void>(std::remove(kFragFile));
}

/// EXPECTATION (audit H-09): the initial load and every successful hot
/// reload advance shader_reload_epoch and swap the id behind the same
/// handle while destroying the old GL program; a failed recompile keeps
/// the old program, does not destroy it, and does not move the epoch.
int check_reload_swaps_program_and_signals_epoch() {
  configure_fake_device();
  remove_files();

  if (!write_file(kVertFile, "#version 330 core\nvoid main() {}\n") ||
      !write_file(kFragFile, "#version 330 core\nvoid main() {}\n")) {
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

  if (!write_file(kFragFile,
                  "#version 330 core\nvoid main() { /* edited */ }\n") ||
      !bump_mtime(kFragFile)) {
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

  if (!write_file(kFragFile, "#version 330 core\nFAILME\n") ||
      !bump_mtime(kFragFile)) {
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

  const int result = check_reload_swaps_program_and_signals_epoch();

  engine::core::shutdown_vfs();
  engine::core::shutdown_logging();
  return result;
}
