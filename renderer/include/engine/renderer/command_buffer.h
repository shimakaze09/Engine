// Declares command buffer types and APIs for the Engine renderer system.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/math/mat4.h"
#include "engine/math/vec3.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/material.h"
#include "engine/renderer/render_view.h"

namespace engine::renderer {

struct RenderDevice;

/// Opaque id of an uploaded GPU mesh (0 = invalid).
struct MeshHandle final {
  std::uint32_t id = 0U;

  friend constexpr bool operator==(const MeshHandle &,
                                   const MeshHandle &) = default;
};

inline constexpr MeshHandle kInvalidMeshHandle{};

// GPU skinning: per-entity bone palettes uploaded once per frame and
// referenced by DrawCommand::skinPalette. One palette fits the 128-joint
// skeleton budget inside the 16 KiB uniform-block size every supported
// backend guarantees. A frame holds one palette per animation component a
// World can hold (checked where palettes are handed out), so no animated
// character goes unposed for want of a slot.
inline constexpr std::size_t kMaxSkinPalettes = 256U;
inline constexpr std::size_t kMaxSkinPaletteJoints = 128U;
inline constexpr std::uint32_t kInvalidSkinPalette = 0xFFFFFFFFU;

/// One entity's skinning matrices (model-space pose times inverse bind).
struct SkinPalette final {
  std::array<math::Mat4, kMaxSkinPaletteJoints> joints{};
  std::uint32_t jointCount = 0U;
};

/// Stores up to kMaxSkinPalettes palettes for the next flush_renderer
/// call; excess palettes are dropped with a log. Pass count 0 to clear.
void set_skin_palettes(const SkinPalette *palettes,
                       std::size_t count) noexcept;
/// Number of palettes currently stored for the next flush.
std::size_t skin_palette_count() noexcept;

// Instancing-ready sort key.
// Bit layout (MSB→LSB):
//   transparent:1 | shadingModel:7 | texture:20 | mesh:20 | depth:16
// Opaque (transparent=0) sorts front-to-back (smaller depth first).
// Transparent (transparent=1) sorts back-to-front (larger depth first).
struct DrawKey final {
  std::uint64_t value = 0U;
};

// The layout above, once. Render prep composes a key from these, the
// builder sorts by them, and the flush partitions draws by them; before
// this the three sites each carried their own copy of the shifts and
// masks, so a field could move in one and not the others.
inline constexpr std::uint64_t kDrawKeyTransparentBit = 1ULL << 63U;
inline constexpr unsigned int kDrawKeyShadingModelShift = 56U;
inline constexpr std::uint64_t kDrawKeyShadingModelMask = 0x7FULL;
inline constexpr unsigned int kDrawKeyTextureShift = 36U;
inline constexpr std::uint64_t kDrawKeyTextureMask = 0xFFFFFULL;
inline constexpr unsigned int kDrawKeyMeshShift = 16U;
inline constexpr std::uint64_t kDrawKeyMeshMask = 0xFFFFFULL;
inline constexpr std::uint64_t kDrawKeyDepthMask = 0xFFFFULL;

/// Whether a key's draw belongs to the transparent half.
constexpr bool draw_key_is_transparent(const DrawKey &key) noexcept {
  return (key.value & kDrawKeyTransparentBit) != 0U;
}

/// The shading model a key selects, as its raw enumerator value. A key
/// can carry a value this build has no run for -- the field is 7 bits
/// wide and kShadingModelCount is smaller -- so a reader validates it
/// with shading_model_is_valid rather than casting blind.
constexpr std::uint8_t draw_key_shading_model(const DrawKey &key) noexcept {
  return static_cast<std::uint8_t>((key.value >> kDrawKeyShadingModelShift) &
                                   kDrawKeyShadingModelMask);
}

/// A key's shading-model field packed for composition.
constexpr std::uint64_t
draw_key_shading_model_bits(ShadingModel model) noexcept {
  return (static_cast<std::uint64_t>(model) & kDrawKeyShadingModelMask)
         << kDrawKeyShadingModelShift;
}

/// The state bits a key carries above its depth, which is what decides
/// whether two draws can share one instanced batch.
constexpr std::uint64_t draw_key_state_bits(const DrawKey &key) noexcept {
  return key.value & ~kDrawKeyDepthMask;
}

/// A key's quantized depth.
constexpr std::uint64_t draw_key_depth(const DrawKey &key) noexcept {
  return key.value & kDrawKeyDepthMask;
}

/// Which passes a draw command feeds. Camera-visible commands
/// carry kPassCamera in the main list; commands render prep culled for
/// the camera but that a shadow sweep or a capture camera can see travel
/// in the auxiliary list with the passes that want them.
inline constexpr std::uint16_t kPassCamera = 1U;
inline constexpr std::uint16_t kPassShadowCaster = 2U;
/// Bit for scene capture `i` is kPassCaptureBase << i.
inline constexpr std::uint16_t kPassCaptureBase = 4U;
/// The eight scene-capture bits together.
inline constexpr std::uint16_t kPassCaptureMask = 0x03FCU;
/// A reflection probe's capture sphere reaches the draw (bit 10, above the
/// capture bits).
inline constexpr std::uint16_t kPassReflectionProbe = 1024U;

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
  std::uint32_t skinPalette = kInvalidSkinPalette;
  std::uint16_t passMask = kPassCamera;
  math::Mat4 modelMatrix = math::Mat4();
};

/// Read-only span over sorted draw commands.
struct CommandBufferView final {
  const DrawCommand *data = nullptr;
  std::uint32_t count = 0U;
};

/// Fixed-capacity draw-command collector; sort before backend flush.
class CommandBufferBuilder final {
public:
  static constexpr std::size_t kMaxDrawCommands = 16384U;

  /// Resets this object back to its reusable empty state.
  void reset() noexcept;
  /// Submits work to the owning buffer or system.
  bool submit(const DrawCommand &command) noexcept;
  /// Appends another builder's commands; false on capacity overflow.
  bool append_from(const CommandBufferBuilder &other) noexcept;
  /// Sorts by DrawKey (opaque front-to-back, transparent back-to-front).
  void sort_by_key() noexcept;
  /// Number of submitted commands.
  std::size_t command_count() const noexcept;
  /// Read-only view of the current commands.
  CommandBufferView view() const noexcept;

private:
  std::array<DrawCommand, kMaxDrawCommands> m_commands =
      std::array<DrawCommand, kMaxDrawCommands>();
  std::size_t m_commandCount = 0U;
};

/// Contiguous run of commands sharing one mesh (instancing batch).
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

struct GpuMeshRegistry;

// Scene light data collected from ECS each frame.
static constexpr std::size_t kMaxDirectionalLights = 4U;
static constexpr std::size_t kMaxPointLights = 128U;
static constexpr std::size_t kMaxSpotLights = 64U;

/// One directional light in the frame's light upload.
struct DirectionalLightData final {
  math::Vec3 direction{};
  math::Vec3 color{};
  float intensity = 0.0F;
};

/// One point light in the frame's light upload.
struct PointLightData final {
  math::Vec3 position{};
  math::Vec3 color{};
  float intensity = 0.0F;
  float radius = 10.0F;
  bool castShadow = false;
};

/// One spot light in the frame's light upload.
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

/// All lights collected from the world for one frame.
struct SceneLightData final {
  std::array<DirectionalLightData, kMaxDirectionalLights> directionalLights{};
  std::size_t directionalLightCount = 0U;
  std::array<PointLightData, kMaxPointLights> pointLights{};
  std::size_t pointLightCount = 0U;
  std::array<SpotLightData, kMaxSpotLights> spotLights{};
  std::size_t spotLightCount = 0U;
};

/// Resolutions and mip counts for an IBL probe bake.
struct ReflectionProbeBakeSettings final {
  std::uint32_t prefilteredFaceSize = 128U;
  std::uint32_t prefilteredMipLevels = 5U;
  std::uint32_t irradianceFaceSize = 32U;
  std::uint32_t brdfLutSize = 512U;
};

/// Source cubemap + settings for bake_reflection_probe.
struct ReflectionProbeBakeRequest final {
  TextureHandle sourceCubemap = kInvalidTextureHandle;
  ReflectionProbeBakeSettings settings{};
};

/// Device textures produced by a probe bake (invalid where a stage was
/// unavailable).
struct ReflectionProbeBakeResult final {
  DeviceTextureHandle sourceCubemapTexture{};
  DeviceTextureHandle prefilteredEnvironmentTexture{};
  DeviceTextureHandle irradianceEnvironmentTexture{};
  DeviceTextureHandle brdfLutTexture{};
  ReflectionProbeBakeSettings settings{};
  bool baked = false;
};

// Scene capture (render-to-texture) requests consumed by flush_renderer.
inline constexpr std::size_t kMaxSceneCaptures = 8U;
inline constexpr std::uint32_t kMinSceneCaptureSize = 16U;
inline constexpr std::uint32_t kMaxSceneCaptureSize = 2048U;

static_assert(kPassCaptureMask ==
                  ((kPassCaptureBase << kMaxSceneCaptures) - kPassCaptureBase),
              "one capture bit per capture slot");
static_assert(kPassReflectionProbe == (kPassCaptureBase << kMaxSceneCaptures),
              "the probe bit sits above the capture bits");

/// One render-to-texture request: capture camera plus target resolution.
struct SceneCaptureRequest final {
  CameraState camera{};
  std::uint32_t width = 256U;
  std::uint32_t height = 256U;
};

/// Clamps and fills the request into a safe runtime range for scene capture.
SceneCaptureRequest
normalize_scene_capture_request(const SceneCaptureRequest &request) noexcept;
/// Stores up to kMaxSceneCaptures requests for the next flush_renderer call;
/// excess requests are dropped with a log. Pass count 0 to disable captures.
void set_scene_capture_requests(const SceneCaptureRequest *requests,
                                std::size_t count) noexcept;
/// Number of capture requests currently stored.
std::size_t scene_capture_request_count() noexcept;
/// LDR color texture rendered for capture slot `index` (request order);
/// invalid until that slot has been rendered by a flush.
DeviceTextureHandle get_scene_capture_texture(std::size_t index) noexcept;
/// Stable texture-system handle for capture slot `index`, usable as a
/// material albedo texture. Invalid until the slot is first requested via
/// set_scene_capture_requests; resolves to "no texture" until the slot's
/// target is created by a flush.
TextureHandle scene_capture_texture_handle(std::size_t index) noexcept;

// Reflection probes: the scene captured into a cubemap at a point and
// baked into an image-based-light environment. A view whose camera is
// inside a probe's box is lit by that probe instead of the sky. When boxes
// overlap, the one with the smaller volume wins, then the lower index.
inline constexpr std::size_t kMaxReflectionProbes = 8U;

/// One reflection probe for the coming frames: where it captures the scene,
/// the box of camera positions it lights, and how its light is sampled.
struct ReflectionProbeRequest final {
  /// Stable identity (the probe entity's persistent id). A probe keeps its
  /// bake while other probes come and go around it.
  std::uint64_t id = 0U;
  math::Vec3 position{};
  /// World-axis-aligned bounds of the probe's box: the camera positions it
  /// lights, and the walls box projection reflects against.
  math::Vec3 boxMin{};
  math::Vec3 boxMax{};
  /// Far plane of the capture. Geometry beyond it is not captured, and
  /// the sky shows through there instead.
  float captureDistance = 10.0F;
  /// Scales the diffuse and specular light the probe gives.
  float intensity = 1.0F;
  /// Parallax-corrects reflections against the box, for a probe that
  /// fills a room.
  bool boxProjection = false;
  /// Cube face size of the capture and its specular prefilter chain, the
  /// chain's length (one roughness step per level), and the diffuse
  /// irradiance face size. Normalized like ReflectionProbeBakeSettings.
  std::uint32_t faceSize = 128U;
  std::uint32_t mipLevels = 5U;
  std::uint32_t irradianceFaceSize = 32U;
};

/// Stores up to kMaxReflectionProbes probes for the coming flushes. More
/// than that are dropped with a warning; count 0 removes every probe. A
/// probe is captured again when its request, the sky or the requested bake
/// generation changes, so resending the same probes every frame bakes
/// nothing new.
void set_reflection_probe_requests(const ReflectionProbeRequest *requests,
                                   std::size_t count) noexcept;
/// Number of probe requests currently stored.
std::size_t reflection_probe_request_count() noexcept;
/// Captures every probe again on the coming frames, for a scene whose
/// geometry or lights changed after its probes were baked. Probes are
/// baked one per frame.
void request_reflection_probe_bake() noexcept;

/// The renderer's state for one probe request.
struct ReflectionProbeStatus final {
  /// Its environment is baked and can light a view.
  bool baked = false;
  /// Captures completed for this probe since it was first requested.
  std::uint32_t bakeCount = 0U;
  /// Where the last capture was taken, and the normalized sizes it used.
  math::Vec3 capturePosition{};
  std::uint32_t faceSize = 0U;
  std::uint32_t mipLevels = 0U;
  std::uint32_t irradianceFaceSize = 0U;
};

/// Writes the state of request `index` to `*out`; false when there is no
/// such request.
bool get_reflection_probe_status(std::size_t index,
                                 ReflectionProbeStatus *out) noexcept;

/// Enumerates distance fog mode values used by the engine.
enum class DistanceFogMode : std::uint8_t {
  Off = 0,
  Linear = 1,
  Exp = 2,
  Exp2 = 3,
};

/// Distance fog: mode, range, density, and color.
struct DistanceFogSettings final {
  DistanceFogMode mode = DistanceFogMode::Exp2;
  float start = 25.0F;
  float end = 150.0F;
  float density = 0.01F;
  math::Vec3 color = math::Vec3(0.55F, 0.65F, 0.75F);
};

/// Height fog: base height, density, falloff, and step count.
struct HeightFogSettings final {
  bool enabled = true;
  float baseHeight = 0.0F;
  float density = 0.015F;
  float falloff = 0.08F;
  std::int32_t stepCount = 8;
};

/// Per-frame renderer counters (draws, triangles, pass timings).
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
  /// False when the device cannot measure pass times, so every gpu*Ms
  /// above reads 0 without being a measurement.
  bool gpuTimingAvailable = false;
};

/// Flushes queued work to the backing runtime system for renderer.
/// `auxiliaryView` carries the camera-culled commands the shadow and
/// capture passes still draw, each tagged by passMask.
/// Renders the Game view: the active camera at the Game view size.
void flush_renderer(CommandBufferView commandBufferView,
                    const GpuMeshRegistry *registry, float timeSeconds,
                    const SceneLightData &lights,
                    CommandBufferView auxiliaryView = {}) noexcept;

/// One view for flush_renderer_view: which, from where, at what size, and
/// what it draws beside the scene.
struct RenderViewDesc final {
  RenderViewId id = RenderViewId::Game;
  CameraState camera{};
  /// Pixels before the render scale; 0 uses game_view_size.
  int width = 0;
  int height = 0;
  /// False draws no scene: the Game view whose panel is hidden still
  /// clears the back buffer for the editor UI and does nothing else.
  bool drawScene = true;
  /// Draws and ages the debug-draw queue (editor gizmos, script lines).
  /// Exactly one view per frame should: the Scene view when one renders,
  /// else the Game view.
  bool drawOverlays = true;
};

/// Renders one view into its own targets. The Game view goes first each
/// frame: it owns the frame's once-only work (GPU profiler frame, quality
/// preset, reflection-probe bakes, scene captures), the back-buffer clear and
/// present, and the frame stats. Another view renders into its own targets
/// only, from `commandBufferView` culled for its camera.
void flush_renderer_view(const RenderViewDesc &view,
                         CommandBufferView commandBufferView,
                         const GpuMeshRegistry *registry, float timeSeconds,
                         const SceneLightData &lights,
                         CommandBufferView auxiliaryView = {}) noexcept;
/// The probe request lighting the last flush of view `id`, or -1 when the
/// sky environment does.
int active_reflection_probe(RenderViewId id) noexcept;
/// Opens a renderer lifetime, re-arming the lazy backend initialization
/// that shutdown_renderer latched off. The backend itself is still built
/// on demand by the first flush, so this call creates no device
/// resources; it exists because the module's owner — engine::bootstrap,
/// which pairs it with shutdown_renderer — is the only thing that can
/// distinguish a new lifetime from a stray flush after teardown (no
/// global may lazily resurrect a subsystem). Calling it twice, or
/// without an intervening shutdown, is harmless.
void initialize_renderer() noexcept;
/// Shuts down the owning system for renderer. Every later flush, probe
/// bake, or other lazy entry point is a logged no-op until the next
/// initialize_renderer, rather than rebuilding the backend against the
/// device and shader system this call destroyed.
void shutdown_renderer() noexcept;

/// The render device for work inside the renderer's lifetime that needs
/// one: created on demand while the renderer is open (the null device on
/// a headless run, which never creates one up front), and null once
/// shutdown_renderer has run, instead of a device brought back behind the
/// renderer's back. Mesh uploads go through here rather than calling
/// initialize_render_device themselves.
const RenderDevice *acquire_render_device() noexcept;

/// Sets the virtual root the built-in renderer shaders load from, with
/// any trailing '/' dropped. A null, empty or overlong root is refused
/// with an Error and the previous root kept: a cut path would name a
/// different directory. No shader loads until a root is set.
bool set_shader_root_path(const char *path) noexcept;

/// The Game view's size in pixels, set by the editor's Game panel. When
/// positive, flush_renderer renders the Game view at this size instead of
/// the window's drawable size.
void set_game_view_size(int width, int height) noexcept;
/// The size the Game view renders at: the override when set, else the
/// window's drawable size.
void game_view_size(int *outWidth, int *outHeight) noexcept;

/// Sets the scene's environment cubemap, or clears it with
/// kInvalidTextureHandle. A set environment is the scene's image-based
/// light (prefiltered specular and diffuse irradiance) under every sky
/// model, and r_sky_model=cubemap also draws it as the sky. The caller owns
/// the texture and must keep it loaded while it is set.
void set_skybox_texture(TextureHandle cubemap) noexcept;
/// Currently bound skybox cubemap handle (may be invalid).
TextureHandle get_skybox_texture() noexcept;

/// Device texture holding a view's final image from its last flush;
/// invalid until that view has rendered.
DeviceTextureHandle get_render_view_texture(RenderViewId view) noexcept;
/// The camera a view last rendered its scene with (a default camera
/// before it first did).
CameraState render_view_camera(RenderViewId view) noexcept;
/// How many frames a view has rendered its scene in this renderer
/// lifetime; a hidden view's count stands still.
std::uint64_t render_view_frame_count(RenderViewId view) noexcept;

/// Live device clip-depth convention (false = GL [-1,1]); projection
/// builders and frustum extraction key off this, defaulting to GL when
/// no device is initialized.
bool device_depth_zero_one() noexcept;

/// Live device render-target texture origin (true = bottom-left, the
/// GL family); consumers sampling engine render targets outside the
/// pass list flip V accordingly.
bool device_target_origin_bottom_left() noexcept;
/// Prefiltered specular environment; invalid until baked/enabled.
DeviceTextureHandle get_prefiltered_environment_texture() noexcept;
/// Diffuse irradiance environment; invalid until baked/enabled.
DeviceTextureHandle get_irradiance_environment_texture() noexcept;
/// Split-sum BRDF LUT; invalid until rendered/enabled.
DeviceTextureHandle get_brdf_lut_texture() noexcept;
/// Clamps and fills settings into a safe runtime range for reflection probe bake settings.
ReflectionProbeBakeSettings normalize_reflection_probe_bake_settings(
    const ReflectionProbeBakeSettings &settings) noexcept;
/// Runs the IBL bake for a probe request; ids are 0 where unavailable.
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
/// Stats recorded by the most recent flush_renderer call.
RendererFrameStats renderer_get_last_frame_stats() noexcept;

// Resets the per-run public renderer state (active camera, scene viewport,
// last frame stats, capture requests, skybox binding) without touching the
// backend; EnginePipeline::teardown calls it so no run residue survives
// into a later run, and shutdown_renderer already calls it.
void reset_renderer_public_state() noexcept;

} // namespace engine::renderer
