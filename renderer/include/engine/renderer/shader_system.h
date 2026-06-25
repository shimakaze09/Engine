// Declares shader system types and APIs for the Engine renderer system.

#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::renderer {

/// Stores render device data used by the engine.
struct RenderDevice;
/// Stores material data used by the engine.
struct Material;

/// Stores shader program handle data used by the engine.
struct ShaderProgramHandle final {
  std::uint32_t id = 0U;

  /// Compares values for equality.
  friend constexpr bool operator==(const ShaderProgramHandle &,
                                   const ShaderProgramHandle &) = default;
};

inline constexpr ShaderProgramHandle kInvalidShaderProgram{};

/// Stores one preprocessor definition requested by a shader variant.
struct ShaderDefine final {
  const char *name = nullptr;
  const char *value = nullptr;
};

/// Stores the deterministic cache key for a shader source and define set.
struct ShaderVariantKey final {
  std::uint64_t value = 0U;

  /// Compares values for equality.
  friend constexpr bool operator==(const ShaderVariantKey &,
                                   const ShaderVariantKey &) = default;
};

/// Stores all source paths and macro definitions needed for one variant.
struct ShaderVariantDesc final {
  const char *vertPath = nullptr;
  const char *fragPath = nullptr;
  const ShaderDefine *defines = nullptr;
  std::size_t defineCount = 0U;
};

inline constexpr std::size_t kMaxMaterialShaderDefines = 5U;

/// Stores shader defines selected from material properties.
struct MaterialShaderVariantSelection final {
  ShaderDefine defines[kMaxMaterialShaderDefines] = {};
  std::size_t defineCount = 0U;
};

/// Initializes the owning system for shader system.
bool initialize_shader_system() noexcept;
/// Shuts down the owning system for shader system.
void shutdown_shader_system() noexcept;

// Load and compile a shader program from VFS paths.  Returns
// kInvalidShaderProgram on failure (logs errors, keeps old program if
// reloading).
ShaderProgramHandle load_shader_program(const char *vertPath,
                                        const char *fragPath) noexcept;

/// Computes a stable key for a shader path pair and unordered define set.
ShaderVariantKey shader_variant_key(const ShaderVariantDesc &desc) noexcept;

/// Loads a shader variant, compiling and caching it when the key is absent.
ShaderProgramHandle load_shader_variant(
    const ShaderVariantDesc &desc) noexcept;

/// Selects shader defines required by a material and mesh skinning state.
MaterialShaderVariantSelection select_material_shader_defines(
    const Material &material, bool skinned) noexcept;

/// Destroys or releases the requested object, handle, or resource for shader program.
void destroy_shader_program(ShaderProgramHandle handle) noexcept;

// Return the underlying GPU program id for a loaded shader program.
// Returns 0 on invalid handle.
std::uint32_t shader_gpu_program(ShaderProgramHandle handle) noexcept;

// Check all loaded programs for source-file changes and recompile as needed.
// Call once per frame.
void check_shader_reload() noexcept;

} // namespace engine::renderer
