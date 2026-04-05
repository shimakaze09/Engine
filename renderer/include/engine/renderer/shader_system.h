#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::renderer {

struct RenderDevice;

struct ShaderProgramHandle final {
  std::uint32_t id = 0U;

  friend constexpr bool operator==(const ShaderProgramHandle &,
                                   const ShaderProgramHandle &) = default;
};

inline constexpr ShaderProgramHandle kInvalidShaderProgram{};

bool initialize_shader_system() noexcept;
void shutdown_shader_system() noexcept;

// Load and compile a shader program from VFS paths.  Returns
// kInvalidShaderProgram on failure (logs errors, keeps old program if
// reloading).
ShaderProgramHandle load_shader_program(const char *vertPath,
                                        const char *fragPath) noexcept;

void destroy_shader_program(ShaderProgramHandle handle) noexcept;

// Return the underlying GPU program id for a loaded shader program.
// Returns 0 on invalid handle.
std::uint32_t shader_gpu_program(ShaderProgramHandle handle) noexcept;

// Check all loaded programs for source-file changes and recompile as needed.
// Call once per frame.
void check_shader_reload() noexcept;

} // namespace engine::renderer
