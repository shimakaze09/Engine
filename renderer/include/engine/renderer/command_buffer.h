// Declares command buffer types and APIs for the Engine renderer system.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/math/mat4.h"
#include "engine/math/vec3.h"
#include "engine/renderer/material.h"

namespace engine::renderer {

/// Stores mesh handle data used by the engine.
struct MeshHandle final {
  std::uint32_t id = 0U;

  /// Compares values for equality.
  friend constexpr bool operator==(const MeshHandle &,
                                   const MeshHandle &) = default;
};

inline constexpr MeshHandle kInvalidMeshHandle{};

// Instancing-ready sort key.
// Bit layout (MSB→LSB):
//   transparent:1 | shader:7 | texture:20 | mesh:20 | depth:16
// Opaque (transparent=0) sorts front-to-back (smaller depth first).
// Transparent (transparent=1) sorts back-to-front (larger depth first).
struct DrawKey final {
  std::uint64_t value = 0U;
};

// Field order is cache-conscious: sort key, hot per-draw identity, and
// material are first. modelMatrix is appended last because it is only
// read once per draw call after the mesh/material state has been set.
struct DrawCommand final {
  DrawKey sortKey{};
  std::uint32_t entity = 0U;
  MeshHandle mesh = kInvalidMeshHandle;
  Material material{};
  float foliageWindStrength = 0.0F;
  float foliageWindFrequency = 1.0F;
  float foliageWindPhase = 0.0F;
  std::uint32_t foliageLodIndex = 0U;
  math::Mat4 modelMatrix = math::Mat4();
};

/// Stores command buffer view data used by the engine.
struct CommandBufferView final {
  const DrawCommand *data = nullptr;
  std::uint32_t count = 0U;
};

/// Owns the command buffer builder behavior and state.
class CommandBufferBuilder final {
public:
  static constexpr std::size_t kMaxDrawCommands = 16384U;

  /// Resets this object back to its reusable empty state.
  void reset() noexcept;
  /// Submits work to the owning buffer or system.
  bool submit(const DrawCommand &command) noexcept;
  /// Handles append from.
  bool append_from(const CommandBufferBuilder &other) noexcept;
  /// Handles sort by key.
  void sort_by_key() noexcept;
  /// Handles command count.
  std::size_t command_count() const noexcept;
  /// Handles view.
  CommandBufferView view() const noexcept;

private:
  std::array<DrawCommand, kMaxDrawCommands> m_commands{};
  std::size_t m_commandCount = 0U;
};

/// Stores static mesh batch data used by the engine.
struct StaticMeshBatch final {
  std::uint32_t first = 0U;
  std::uint32_t count = 0U;
};

/// Builds the requested runtime data for static mesh batches.
std::size_t build_static_mesh_batches(CommandBufferView commandBufferView,
                                      std::size_t start,
                                      std::size_t end,
                                      StaticMeshBatch *batches,
                                      std::size_t batchCapacity) noexcept;

/// Stores gpu mesh registry data used by the engine.
struct GpuMeshRegistry;

// Scene light data collected from ECS each frame.
static constexpr std::size_t kMaxDirectionalLights = 4U;
static constexpr std::size_t kMaxPointLights = 128U;
static constexpr std::size_t kMaxSpotLights = 64U;

/// Stores directional light data used by the engine.
struct DirectionalLightData final {
  math::Vec3 direction{};
  math::Vec3 color{};
  float intensity = 0.0F;
};

/// Stores point light data used by the engine.
struct PointLightData final {
  math::Vec3 position{};
  math::Vec3 color{};
  float intensity = 0.0F;
  float radius = 10.0F;
  bool castShadow = false;
};

/// Stores spot light data used by the engine.
struct SpotLightData final {
  math::Vec3 position{};
  math::Vec3 direction{};
  math::Vec3 color{};
  float intensity = 0.0F;
  float radius = 10.0F;
  float innerConeAngle = 0.3491F; // ~20 degrees
  float outerConeAngle = 0.5236F; // ~30 degrees
  bool castShadow = false;
};

/// Stores scene light data used by the engine.
struct SceneLightData final {
  std::array<DirectionalLightData, kMaxDirectionalLights> directionalLights{};
  std::size_t directionalLightCount = 0U;
  std::array<PointLightData, kMaxPointLights> pointLights{};
  std::size_t pointLightCount = 0U;
  std::array<SpotLightData, kMaxSpotLights> spotLights{};
  std::size_t spotLightCount = 0U;
};

/// Stores reflection probe bake settings data used by the engine.
struct ReflectionProbeBakeSettings final {
  std::uint32_t prefilteredFaceSize = 128U;
  std::uint32_t prefilteredMipLevels = 5U;
  std::uint32_t irradianceFaceSize = 32U;
  std::uint32_t brdfLutSize = 512U;
};

/// Stores reflection probe bake request data used by the engine.
struct ReflectionProbeBakeRequest final {
  TextureHandle sourceCubemap = kInvalidTextureHandle;
  ReflectionProbeBakeSettings settings{};
};

/// Stores reflection probe bake result data used by the engine.
struct ReflectionProbeBakeResult final {
  std::uint32_t sourceCubemapTexture = 0U;
  std::uint32_t prefilteredEnvironmentTexture = 0U;
  std::uint32_t irradianceEnvironmentTexture = 0U;
  std::uint32_t brdfLutTexture = 0U;
  ReflectionProbeBakeSettings settings{};
  bool baked = false;
};

/// Enumerates distance fog mode values used by the engine.
enum class DistanceFogMode : std::uint8_t {
  Off = 0,
  Linear = 1,
  Exp = 2,
  Exp2 = 3,
};

/// Stores distance fog settings data used by the engine.
struct DistanceFogSettings final {
  DistanceFogMode mode = DistanceFogMode::Exp2;
  float start = 25.0F;
  float end = 150.0F;
  float density = 0.01F;
  math::Vec3 color = math::Vec3(0.55F, 0.65F, 0.75F);
};

/// Stores height fog settings data used by the engine.
struct HeightFogSettings final {
  bool enabled = true;
  float baseHeight = 0.0F;
  float density = 0.015F;
  float falloff = 0.08F;
  std::int32_t stepCount = 8;
};

/// Stores renderer frame stats data used by the engine.
struct RendererFrameStats final {
  std::uint32_t drawCalls = 0U;
  std::uint64_t triangleCount = 0U;
  float gpuSceneMs = 0.0F;
  float gpuTonemapMs = 0.0F;
  float gpuGBufferMs = 0.0F;
  float gpuDeferredLightMs = 0.0F;
  float gpuBloomMs = 0.0F;
  float gpuSsaoMs = 0.0F;
  float gpuShadowMapMs = 0.0F;
  float gpuSpotShadowMs = 0.0F;
  float gpuPointShadowMs = 0.0F;
  float gpuAutoExposureMs = 0.0F;
};

/// Flushes queued work to the backing runtime system for renderer.
void flush_renderer(CommandBufferView commandBufferView,
                    const GpuMeshRegistry *registry, float timeSeconds,
                    const SceneLightData &lights) noexcept;
/// Shuts down the owning system for renderer.
void shutdown_renderer() noexcept;

/// Sets the virtual root used for built-in renderer shaders.
void set_shader_root_path(const char *path) noexcept;

// Optional scene viewport override from editor UI. When set to positive
// values, flush_renderer uses this size for projection and pass resources
// instead of the full SDL drawable size.
void set_scene_viewport_size(int width, int height) noexcept;

/// Sets the active skybox cubemap. Pass kInvalidTextureHandle to disable it.
void set_skybox_texture(TextureHandle cubemap) noexcept;
/// Returns the requested value for skybox texture.
TextureHandle get_skybox_texture() noexcept;

/// Returns the GPU texture ID of the tonemapped scene (final color).
/// Valid after the first flush_renderer call. Returns 0 if not yet available.
std::uint32_t get_scene_viewport_texture() noexcept;
/// Returns the requested value for prefiltered environment texture.
std::uint32_t get_prefiltered_environment_texture() noexcept;
/// Returns the requested value for irradiance environment texture.
std::uint32_t get_irradiance_environment_texture() noexcept;
/// Returns the requested value for brdf lut texture.
std::uint32_t get_brdf_lut_texture() noexcept;
/// Clamps and fills settings into a safe runtime range for reflection probe bake settings.
ReflectionProbeBakeSettings normalize_reflection_probe_bake_settings(
    const ReflectionProbeBakeSettings &settings) noexcept;
/// Handles bake reflection probe.
ReflectionProbeBakeResult
bake_reflection_probe(const ReflectionProbeBakeRequest &request) noexcept;
/// Parses text into the engine representation for distance fog mode.
DistanceFogMode parse_distance_fog_mode(const char *mode) noexcept;
/// Parses text into the engine representation for distance fog color.
bool parse_distance_fog_color(const char *value,
                              math::Vec3 *colorOut) noexcept;
/// Clamps and fills settings into a safe runtime range for distance fog settings.
DistanceFogSettings
normalize_distance_fog_settings(const DistanceFogSettings &settings) noexcept;
/// Clamps and fills settings into a safe runtime range for height fog settings.
HeightFogSettings
normalize_height_fog_settings(const HeightFogSettings &settings) noexcept;
/// Handles renderer get last frame stats.
RendererFrameStats renderer_get_last_frame_stats() noexcept;

} // namespace engine::renderer
