#include "engine/renderer/shader_system.h"

#include <cstdint>
#include <cstring>

#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/renderer/render_device.h"

namespace engine::renderer {

namespace {

constexpr std::size_t kMaxShaderPrograms = 64U;
constexpr std::size_t kMaxPathLength = 128U;

void safe_copy_path(char *dst, const char *src) noexcept {
  std::size_t i = 0U;
  while ((i < kMaxPathLength - 1U) && (src[i] != '\0')) {
    dst[i] = src[i];
    ++i;
  }
  dst[i] = '\0';
}

struct ShaderEntry final {
  bool active = false;
  char vertPath[kMaxPathLength] = {};
  char fragPath[kMaxPathLength] = {};
  std::uint32_t gpuProgram = 0U;
  std::int64_t vertMtime = 0;
  std::int64_t fragMtime = 0;
};

ShaderEntry g_entries[kMaxShaderPrograms] = {};
bool g_initialized = false;

std::uint32_t compile_program_from_source(const char *vertSource,
                                          const char *fragSource) noexcept {
  const RenderDevice *dev = render_device();
  if (dev == nullptr) {
    return 0U;
  }

  const std::uint32_t vs = dev->create_shader(kShaderStageVertex, vertSource);
  const std::uint32_t fs = dev->create_shader(kShaderStageFragment, fragSource);

  if ((vs == 0U) || (fs == 0U)) {
    if (vs != 0U) {
      dev->destroy_shader(vs);
    }
    if (fs != 0U) {
      dev->destroy_shader(fs);
    }
    return 0U;
  }

  const std::uint32_t program = dev->link_program(vs, fs);
  dev->destroy_shader(vs);
  dev->destroy_shader(fs);
  return program;
}

bool try_reload_entry(ShaderEntry &entry) noexcept {
  char *vertSource = nullptr;
  std::size_t vertSize = 0U;
  if (!core::vfs_read_text(entry.vertPath, &vertSource, &vertSize)) {
    core::log_message(
        core::LogLevel::Error, "shader", "failed to read vertex shader");
    return false;
  }

  char *fragSource = nullptr;
  std::size_t fragSize = 0U;
  if (!core::vfs_read_text(entry.fragPath, &fragSource, &fragSize)) {
    core::vfs_free(vertSource);
    core::log_message(
        core::LogLevel::Error, "shader", "failed to read fragment shader");
    return false;
  }

  const std::uint32_t newProgram =
      compile_program_from_source(vertSource, fragSource);
  core::vfs_free(vertSource);
  core::vfs_free(fragSource);

  if (newProgram == 0U) {
    core::log_message(core::LogLevel::Error,
                      "shader",
                      "shader compilation failed — keeping old program");
    return false;
  }

  // Destroy old program and swap in the new one.
  const RenderDevice *dev = render_device();
  if ((dev != nullptr) && (entry.gpuProgram != 0U)) {
    dev->destroy_program(entry.gpuProgram);
  }

  entry.gpuProgram = newProgram;
  entry.vertMtime = core::vfs_file_mtime(entry.vertPath);
  entry.fragMtime = core::vfs_file_mtime(entry.fragPath);
  return true;
}

} // namespace

bool initialize_shader_system() noexcept {
  if (g_initialized) {
    return true;
  }

  for (auto &e : g_entries) {
    e = ShaderEntry{};
  }

  g_initialized = true;
  return true;
}

void shutdown_shader_system() noexcept {
  if (!g_initialized) {
    return;
  }

  const RenderDevice *dev = render_device();
  for (auto &e : g_entries) {
    if (e.active && (e.gpuProgram != 0U) && (dev != nullptr)) {
      dev->destroy_program(e.gpuProgram);
    }
    e = ShaderEntry{};
  }

  g_initialized = false;
}

ShaderProgramHandle load_shader_program(const char *vertPath,
                                        const char *fragPath) noexcept {
  if ((vertPath == nullptr) || (fragPath == nullptr) || !g_initialized) {
    return kInvalidShaderProgram;
  }

  // Find a free slot.
  std::size_t slot = kMaxShaderPrograms;
  for (std::size_t i = 0U; i < kMaxShaderPrograms; ++i) {
    if (!g_entries[i].active) {
      slot = i;
      break;
    }
  }

  if (slot == kMaxShaderPrograms) {
    core::log_message(
        core::LogLevel::Error, "shader", "shader program registry full");
    return kInvalidShaderProgram;
  }

  ShaderEntry &entry = g_entries[slot];
  safe_copy_path(entry.vertPath, vertPath);
  safe_copy_path(entry.fragPath, fragPath);
  entry.active = true;

  if (!try_reload_entry(entry)) {
    entry = ShaderEntry{};
    return kInvalidShaderProgram;
  }

  // Handle id is slot + 1 so that 0 remains invalid.
  return ShaderProgramHandle{static_cast<std::uint32_t>(slot + 1U)};
}

void destroy_shader_program(ShaderProgramHandle handle) noexcept {
  if ((handle.id == 0U) || (handle.id > kMaxShaderPrograms) || !g_initialized) {
    return;
  }

  ShaderEntry &entry = g_entries[handle.id - 1U];
  if (!entry.active) {
    return;
  }

  const RenderDevice *dev = render_device();
  if ((dev != nullptr) && (entry.gpuProgram != 0U)) {
    dev->destroy_program(entry.gpuProgram);
  }

  entry = ShaderEntry{};
}

std::uint32_t shader_gpu_program(ShaderProgramHandle handle) noexcept {
  if ((handle.id == 0U) || (handle.id > kMaxShaderPrograms) || !g_initialized) {
    return 0U;
  }

  const ShaderEntry &entry = g_entries[handle.id - 1U];
  return entry.active ? entry.gpuProgram : 0U;
}

void check_shader_reload() noexcept {
  if (!g_initialized) {
    return;
  }

  for (auto &entry : g_entries) {
    if (!entry.active) {
      continue;
    }

    const std::int64_t vertMtime = core::vfs_file_mtime(entry.vertPath);
    const std::int64_t fragMtime = core::vfs_file_mtime(entry.fragPath);

    if ((vertMtime != entry.vertMtime) || (fragMtime != entry.fragMtime)) {
      core::log_message(
          core::LogLevel::Info, "shader", "reloading modified shader");
      try_reload_entry(entry);
    }
  }
}

} // namespace engine::renderer
