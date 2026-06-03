// Declares shader system types and APIs for the Engine renderer system.

#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::renderer {

/// Stores render device data used by the engine.
struct RenderDevice;

/// Stores shader program handle data used by the engine.
struct ShaderProgramHandle final {
  std::uint32_t id = 0U;

  /// Compares values for equality.
  friend constexpr bool operator==(const ShaderProgramHandle &,
                                   const ShaderProgramHandle &) = default;
};

inline constexpr ShaderProgramHandle kInvalidShaderProgram{};

/// Initializes the owning system for shader system.
bool initialize_shader_system() noexcept;
/// Shuts down the owning system for shader system.
void shutdown_shader_system() noexcept;

// Load and compile a shader program from VFS paths.  Returns
// kInvalidShaderProgram on failure (logs errors, keeps old program if
// reloading).
ShaderProgramHandle load_shader_program(const char *vertPath,
                                        const char *fragPath) noexcept;

/// Destroys or releases the requested object, handle, or resource for shader program.
void destroy_shader_program(ShaderProgramHandle handle) noexcept;

// Return the underlying GPU program id for a loaded shader program.
// Returns 0 on invalid handle.
std::uint32_t shader_gpu_program(ShaderProgramHandle handle) noexcept;

// Check all loaded programs for source-file changes and recompile as needed.
// Call once per frame.
void check_shader_reload() noexcept;

} // namespace engine::renderer
