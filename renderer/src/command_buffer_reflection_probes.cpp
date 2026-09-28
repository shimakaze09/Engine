// Implements reflection probes: the requests the runtime sends each frame,
// the capture of a probe's surroundings into a cubemap and its bake into an
// image-based-light environment, and the choice of environment per view.
//
// The lighting shaders have one environment slot (all sixteen sampler
// units are taken), so a view is lit by one environment: the probe whose
// box holds its camera, else the sky. That is the zone model older engines
// use for cubemaps (Source's env_cubemap areas); per-pixel blending between
// probes needs the sampler budget a clustered probe array would free.
// Probes bake on demand like Unity's baked and Godot's "Once" probes: when
// they appear, move, change settings, see a new sky, or a bake is asked for.

#include "command_buffer_reflection_probes.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "command_buffer_ibl.h"
#include "command_buffer_sky.h"
#include "engine/core/logging.h"
#include "engine/math/mat4.h"
#include "engine/math/transform.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/command_buffer.h"

namespace engine::renderer {

namespace {

constexpr const char *kProbeLogChannel = "renderer";
constexpr float kProbeNearPlane = 0.05F;
constexpr float kMinCaptureDistance = 0.1F;
constexpr float kQuarterTurn = 1.57079632679F;

/// The request `request` becomes once its values are in range: the box
/// ordered, the capture reaching past the near plane, the intensity
/// non-negative, and the sizes a bake can use.
ReflectionProbeRequest
normalize_probe_request(const ReflectionProbeRequest &request) noexcept {
  ReflectionProbeRequest normalized = request;
  normalized.boxMin = math::Vec3(std::min(request.boxMin.x, request.boxMax.x),
                                 std::min(request.boxMin.y, request.boxMax.y),
                                 std::min(request.boxMin.z, request.boxMax.z));
  normalized.boxMax = math::Vec3(std::max(request.boxMin.x, request.boxMax.x),
                                 std::max(request.boxMin.y, request.boxMax.y),
                                 std::max(request.boxMin.z, request.boxMax.z));
  normalized.captureDistance =
      std::max(request.captureDistance, kMinCaptureDistance);
  normalized.intensity = std::max(request.intensity, 0.0F);
  ReflectionProbeBakeSettings settings{};
  settings.prefilteredFaceSize = request.faceSize;
  settings.prefilteredMipLevels = request.mipLevels;
  settings.irradianceFaceSize = request.irradianceFaceSize;
  settings = normalize_reflection_probe_bake_settings(settings);
  normalized.faceSize = settings.prefilteredFaceSize;
  normalized.mipLevels = settings.prefilteredMipLevels;
  normalized.irradianceFaceSize = settings.irradianceFaceSize;
  return normalized;
}

bool same_vec3(const math::Vec3 &a, const math::Vec3 &b) noexcept {
  return (a.x == b.x) && (a.y == b.y) && (a.z == b.z);
}

/// True when two normalized requests capture and bake the same thing.
bool same_request(const ReflectionProbeRequest &a,
                  const ReflectionProbeRequest &b) noexcept {
  return (a.id == b.id) && same_vec3(a.position, b.position) &&
         same_vec3(a.boxMin, b.boxMin) && same_vec3(a.boxMax, b.boxMax) &&
         (a.captureDistance == b.captureDistance) &&
         (a.intensity == b.intensity) && (a.boxProjection == b.boxProjection) &&
         (a.faceSize == b.faceSize) && (a.mipLevels == b.mipLevels) &&
         (a.irradianceFaceSize == b.irradianceFaceSize);
}

void destroy_texture(const RenderDevice *dev,
                     DeviceTextureHandle *texture) noexcept {
  if ((*texture != kInvalidDeviceTexture) && (dev != nullptr) &&
      (dev->destroy_texture != nullptr)) {
    dev->destroy_texture(*texture);
  }
  *texture = kInvalidDeviceTexture;
}

/// Releases a slot's textures and frees it for another probe.
void release_slot(ReflectionProbeSlot &slot) noexcept {
  const RenderDevice *dev = render_device();
  destroy_texture(dev, &slot.captureCube);
  destroy_texture(dev, &slot.captureDepth);
  release_prefiltered_environment(slot.environment);
  release_irradiance_environment(slot.environment);
  slot = ReflectionProbeSlot{};
}

/// The slot holding probe `id`, or nullptr.
ReflectionProbeSlot *find_slot(BackendState &backend,
                               std::uint64_t id) noexcept {
  for (ReflectionProbeSlot &slot : backend.reflectionProbes) {
    if (slot.used && (slot.id == id)) {
      return &slot;
    }
  }
  return nullptr;
}

const ReflectionProbeSlot *find_slot(const BackendState &backend,
                                     std::uint64_t id) noexcept {
  for (const ReflectionProbeSlot &slot : backend.reflectionProbes) {
    if (slot.used && (slot.id == id)) {
      return &slot;
    }
  }
  return nullptr;
}

/// Frees the slots of probes no longer requested, then gives every
/// requested probe without one a free slot. There are as many slots as
/// requests can be, so every request ends with a slot.
void assign_slots(BackendState &backend, const ReflectionProbeRequest *requests,
                  std::size_t count) noexcept {
  for (ReflectionProbeSlot &slot : backend.reflectionProbes) {
    if (!slot.used) {
      continue;
    }
    bool requested = false;
    for (std::size_t i = 0U; (i < count) && !requested; ++i) {
      requested = (requests[i].id == slot.id);
    }
    if (!requested) {
      release_slot(slot);
    }
  }
  for (std::size_t i = 0U; i < count; ++i) {
    if (find_slot(backend, requests[i].id) != nullptr) {
      continue;
    }
    for (ReflectionProbeSlot &slot : backend.reflectionProbes) {
      if (!slot.used) {
        slot.used = true;
        slot.id = requests[i].id;
        break;
      }
    }
  }
}

/// True when the slot's last capture no longer matches what it would
/// capture now.
bool capture_out_of_date(const ReflectionProbeSlot &slot,
                         const ReflectionProbeRequest &request,
                         TextureHandle sky, SkyModel skyModel,
                         std::uint32_t generation) noexcept {
  return !slot.captured || !same_request(slot.capturedFor, request) ||
         (slot.capturedSky != sky) ||
         (slot.capturedSkyModel != static_cast<std::uint8_t>(skyModel)) ||
         (slot.capturedGeneration != generation);
}

/// Creates the slot's capture cube and depth buffer at `faceSize`, replacing
/// ones of another size; false (logged) when the device refuses either.
bool ensure_capture_targets(ReflectionProbeSlot &slot, const RenderDevice *dev,
                            int faceSize) noexcept {
  if ((slot.captureCube != kInvalidDeviceTexture) &&
      (slot.captureDepth != kInvalidDeviceTexture) &&
      (slot.captureFaceSize == faceSize)) {
    return true;
  }
  destroy_texture(dev, &slot.captureCube);
  destroy_texture(dev, &slot.captureDepth);
  slot.captureFaceSize = 0;

  // HDR, one level: the environment bake reads it as a sky cubemap.
  TextureDesc cubeDesc{};
  cubeDesc.kind = TextureKind::Cube;
  cubeDesc.format = TextureFormat::RGB16F;
  cubeDesc.width = faceSize;
  cubeDesc.mipLevels = 1;
  cubeDesc.filter = TextureFilter::Linear;
  cubeDesc.wrap = TextureWrap::ClampEdge;
  slot.captureCube = dev->create_texture(cubeDesc);

  TextureDesc depthDesc{};
  depthDesc.kind = TextureKind::Tex2D;
  depthDesc.format = TextureFormat::Depth24;
  depthDesc.width = faceSize;
  depthDesc.height = faceSize;
  depthDesc.filter = TextureFilter::Nearest;
  depthDesc.wrap = TextureWrap::ClampEdge;
  slot.captureDepth = dev->create_texture(depthDesc);

  if ((slot.captureCube == kInvalidDeviceTexture) ||
      (slot.captureDepth == kInvalidDeviceTexture)) {
    core::log_message(core::LogLevel::Warning, kProbeLogChannel,
                      "reflection probe capture textures unavailable; the "
                      "probe is lit by the sky");
    destroy_texture(dev, &slot.captureCube);
    destroy_texture(dev, &slot.captureDepth);
    return false;
  }
  slot.captureFaceSize = faceSize;
  return true;
}

/// Draws the sky the selected sky model shows onto the bound face.
void draw_capture_sky(const OffscreenSceneInputs &inputs,
                      DeviceTextureHandle skyCubemap, SkyModel skyModel,
                      const math::Mat4 &view,
                      const math::Mat4 &skyProjection) noexcept {
  const BackendState &backend = *inputs.backend;
  RendererFrameStats &stats = *inputs.frameStats;
  if ((skyModel == SkyModel::Cubemap) &&
      (skyCubemap != kInvalidDeviceTexture)) {
    draw_skybox(backend, inputs.dev, view, skyProjection, skyCubemap, stats);
  } else if ((skyModel == SkyModel::Hosek) && backend.hosekSkyAvailable) {
    draw_hosek_sky(backend, inputs.dev, view, skyProjection, *inputs.lights,
                   stats);
  } else if (((skyModel == SkyModel::Preetham) ||
              (skyModel == SkyModel::Hosek)) &&
             backend.preethamSkyAvailable) {
    draw_preetham_sky(backend, inputs.dev, view, skyProjection, *inputs.lights,
                      stats);
  }
}

/// Renders the six faces of the probe's surroundings into its capture
/// cube: sky first, then the scene within the capture distance, in the
/// cubemap face order and orientation the environment bake samples.
bool capture_faces(const OffscreenSceneInputs &inputs,
                   ReflectionProbeSlot &slot,
                   const ReflectionProbeRequest &request,
                   DeviceTextureHandle skyCubemap, SkyModel skyModel,
                   const IblSelection &skyIbl) noexcept {
  const RenderDevice *dev = inputs.dev;
  const int faceSize = static_cast<int>(request.faceSize);
  if (!ensure_capture_targets(slot, dev, faceSize)) {
    return false;
  }

  // +X, -X, +Y, -Y, +Z, -Z with the cubemap convention's up vectors, the
  // same as the environment bake and the point shadow faces.
  struct FaceBasis final {
    math::Vec3 forward;
    math::Vec3 up;
  };
  constexpr std::array<FaceBasis, 6> kFaces = {{
      {math::Vec3(1.0F, 0.0F, 0.0F), math::Vec3(0.0F, -1.0F, 0.0F)},
      {math::Vec3(-1.0F, 0.0F, 0.0F), math::Vec3(0.0F, -1.0F, 0.0F)},
      {math::Vec3(0.0F, 1.0F, 0.0F), math::Vec3(0.0F, 0.0F, 1.0F)},
      {math::Vec3(0.0F, -1.0F, 0.0F), math::Vec3(0.0F, 0.0F, -1.0F)},
      {math::Vec3(0.0F, 0.0F, 1.0F), math::Vec3(0.0F, -1.0F, 0.0F)},
      {math::Vec3(0.0F, 0.0F, -1.0F), math::Vec3(0.0F, -1.0F, 0.0F)},
  }};

  CameraState camera{};
  camera.position = request.position;
  camera.fovRadians = kQuarterTurn;
  camera.nearPlane = kProbeNearPlane;
  camera.farPlane = request.captureDistance;
  const math::Mat4 projection = camera_projection_matrix(camera, 1.0F);
  const math::Mat4 skyProjection = sky_projection_matrix(camera, 1.0F);

  bool captured = true;
  for (std::size_t face = 0U; face < kFaces.size(); ++face) {
    RenderTargetDesc desc{};
    desc.colorCount = 1U;
    desc.colors[0].texture = slot.captureCube;
    desc.colors[0].face = static_cast<CubeFace>(face);
    desc.depth.texture = slot.captureDepth;
    const RenderTargetHandle target = dev->create_render_target(desc);
    if (target.value == 0U) {
      captured = false;
      break;
    }
    dev->bind_render_target(target);
    // After the bind: the viewport applies to the view the bind claimed.
    dev->set_viewport(0, 0, faceSize, faceSize);
    dev->apply_render_state(RenderState{DepthTest::Less, true,
                                        BlendMode::Disabled, CullMode::None});
    dev->clear(ClearFlags::ColorDepth, 0.0F, 0.0F, 0.0F, 1.0F);

    OffscreenCamera faceCamera{};
    faceCamera.position = request.position;
    faceCamera.view = math::look_at(
        request.position, math::add(request.position, kFaces[face].forward),
        kFaces[face].up);
    faceCamera.projection = projection;
    faceCamera.auxiliaryMask = kPassReflectionProbe;
    faceCamera.renderTarget = slot.captureCube;
    // Both sides of every triangle: a probe inside a single-sided mesh (a
    // room built from planes facing outward) still sees its walls, and a
    // bake that runs once can afford the extra fill.
    faceCamera.cull = CullMode::None;

    draw_capture_sky(inputs, skyCubemap, skyModel, faceCamera.view,
                     skyProjection);
    draw_offscreen_scene(inputs, faceCamera, skyIbl);
    dev->bind_render_target(kBackBufferTarget);
    dev->destroy_render_target(target);
  }
  dev->bind_render_target(kBackBufferTarget);
  dev->apply_render_state(
      RenderState{DepthTest::Less, true, BlendMode::Disabled, CullMode::Back});
  if (!captured) {
    core::log_message(core::LogLevel::Warning, kProbeLogChannel,
                      "reflection probe face target unavailable; the probe "
                      "is lit by the sky");
  }
  return captured;
}

/// Captures one probe and bakes its environment; records what the capture
/// was taken for even when it fails, so a device that cannot capture is
/// not asked again every frame.
void bake_probe(const OffscreenSceneInputs &inputs, ReflectionProbeSlot &slot,
                const ReflectionProbeRequest &request,
                DeviceTextureHandle skyCubemap, SkyModel skyModel,
                const IblSelection &skyIbl, std::uint32_t generation) noexcept {
  BackendState &backend = *inputs.backend;
  slot.captured = true;
  slot.capturedFor = request;
  slot.capturedSky = renderer_context().activeSkyboxTexture;
  slot.capturedSkyModel = static_cast<std::uint8_t>(skyModel);
  slot.capturedGeneration = generation;

  if (!capture_faces(inputs, slot, request, skyCubemap, skyModel, skyIbl)) {
    release_prefiltered_environment(slot.environment);
    release_irradiance_environment(slot.environment);
    return;
  }
  ++slot.captureVersion;
  IblBakeSource source{};
  source.cubemap = slot.captureCube;
  source.version = slot.captureVersion;
  ReflectionProbeBakeSettings settings{};
  settings.prefilteredFaceSize = request.faceSize;
  settings.prefilteredMipLevels = request.mipLevels;
  settings.irradianceFaceSize = request.irradianceFaceSize;
  const DeviceTextureHandle prefiltered = ensure_prefiltered_environment(
      backend, inputs.dev, slot.environment, source, settings);
  const DeviceTextureHandle irradiance = ensure_irradiance_environment(
      backend, inputs.dev, slot.environment, source, settings);
  if ((prefiltered != kInvalidDeviceTexture) &&
      (irradiance != kInvalidDeviceTexture)) {
    ++slot.bakeCount;
  }
}

/// True when a probe's environment can light a view.
bool slot_baked(const ReflectionProbeSlot &slot) noexcept {
  return (slot.captureVersion > 0U) &&
         (slot.environment.prefilteredTexture != kInvalidDeviceTexture) &&
         (slot.environment.irradianceTexture != kInvalidDeviceTexture) &&
         (slot.environment.prefilteredSource.version == slot.captureVersion);
}

bool box_contains(const ReflectionProbeRequest &request,
                  const math::Vec3 &point) noexcept {
  return (point.x >= request.boxMin.x) && (point.x <= request.boxMax.x) &&
         (point.y >= request.boxMin.y) && (point.y <= request.boxMax.y) &&
         (point.z >= request.boxMin.z) && (point.z <= request.boxMax.z);
}

float box_volume(const ReflectionProbeRequest &request) noexcept {
  return (request.boxMax.x - request.boxMin.x) *
         (request.boxMax.y - request.boxMin.y) *
         (request.boxMax.z - request.boxMin.z);
}

} // namespace

void set_reflection_probe_requests(const ReflectionProbeRequest *requests,
                                   std::size_t count) noexcept {
  RendererContext &context = renderer_context();
  if ((requests == nullptr) && (count > 0U)) {
    core::log_message(core::LogLevel::Error, kProbeLogChannel,
                      "reflection probe request array is null");
    count = 0U;
  }
  if (count > kMaxReflectionProbes) {
    core::log_message(core::LogLevel::Warning, kProbeLogChannel,
                      "reflection probe requests exceed the probe slots; "
                      "extra probes dropped");
    count = kMaxReflectionProbes;
  }
  for (std::size_t i = 0U; i < count; ++i) {
    context.reflectionProbeRequests[i] = normalize_probe_request(requests[i]);
  }
  context.reflectionProbeRequestCount = count;
  assign_slots(context.backend, context.reflectionProbeRequests.data(), count);
}

std::size_t reflection_probe_request_count() noexcept {
  return renderer_context().reflectionProbeRequestCount;
}

bool get_reflection_probe_request(std::size_t index,
                                  ReflectionProbeRequest *out) noexcept {
  const RendererContext &context = renderer_context();
  if ((out == nullptr) || (index >= context.reflectionProbeRequestCount)) {
    return false;
  }
  *out = context.reflectionProbeRequests[index];
  return true;
}

void request_reflection_probe_bake() noexcept {
  ++renderer_context().reflectionProbeBakeGeneration;
}

bool get_reflection_probe_status(std::size_t index,
                                 ReflectionProbeStatus *out) noexcept {
  const RendererContext &context = renderer_context();
  if ((out == nullptr) || (index >= context.reflectionProbeRequestCount)) {
    return false;
  }
  const ReflectionProbeRequest &request =
      context.reflectionProbeRequests[index];
  ReflectionProbeStatus status{};
  const ReflectionProbeSlot *slot = find_slot(context.backend, request.id);
  if (slot != nullptr) {
    status.baked = slot_baked(*slot);
    status.bakeCount = slot->bakeCount;
    if (slot->captured) {
      status.capturePosition = slot->capturedFor.position;
      status.faceSize = slot->capturedFor.faceSize;
      status.mipLevels = slot->capturedFor.mipLevels;
      status.irradianceFaceSize = slot->capturedFor.irradianceFaceSize;
    }
  }
  *out = status;
  return true;
}

int active_reflection_probe(RenderViewId id) noexcept {
  const std::size_t viewIndex = render_view_index(id);
  if (viewIndex >= kMaxRenderViews) {
    return -1;
  }
  return renderer_context().activeReflectionProbe[viewIndex];
}

void bake_pending_reflection_probe(const OffscreenSceneInputs &inputs,
                                   const IblSelection &skyIbl,
                                   DeviceTextureHandle skyCubemap) noexcept {
  RendererContext &context = renderer_context();
  if ((inputs.backend == nullptr) || (inputs.dev == nullptr) ||
      (inputs.dev->create_render_target == nullptr) ||
      (inputs.dev->create_texture == nullptr)) {
    return;
  }
  const SkyModel skyModel = selected_sky_model();
  for (std::size_t i = 0U; i < context.reflectionProbeRequestCount; ++i) {
    const ReflectionProbeRequest &request = context.reflectionProbeRequests[i];
    ReflectionProbeSlot *slot = find_slot(*inputs.backend, request.id);
    if ((slot == nullptr) ||
        !capture_out_of_date(*slot, request, context.activeSkyboxTexture,
                             skyModel, context.reflectionProbeBakeGeneration)) {
      continue;
    }
    bake_probe(inputs, *slot, request, skyCubemap, skyModel, skyIbl,
               context.reflectionProbeBakeGeneration);
    return;
  }
}

IblSelection select_view_environment(const BackendState &backend,
                                     std::size_t viewIndex,
                                     const math::Vec3 &cameraPosition,
                                     const IblSelection &skyIbl) noexcept {
  RendererContext &context = renderer_context();
  int chosen = -1;
  float chosenVolume = 0.0F;
  const ReflectionProbeSlot *chosenSlot = nullptr;
  for (std::size_t i = 0U; i < context.reflectionProbeRequestCount; ++i) {
    const ReflectionProbeRequest &request = context.reflectionProbeRequests[i];
    const ReflectionProbeSlot *slot = find_slot(backend, request.id);
    if ((slot == nullptr) || !slot_baked(*slot) ||
        !box_contains(request, cameraPosition)) {
      continue;
    }
    const float volume = box_volume(request);
    if ((chosen < 0) || (volume < chosenVolume)) {
      chosen = static_cast<int>(i);
      chosenVolume = volume;
      chosenSlot = slot;
    }
  }
  if (viewIndex < kMaxRenderViews) {
    context.activeReflectionProbe[viewIndex] = chosen;
  }
  if ((chosenSlot == nullptr) ||
      (backend.brdfLutTexture == kInvalidDeviceTexture)) {
    return skyIbl;
  }

  // The environment and the box it was captured for: a probe that moved
  // since still lights from where it was captured until it is baked again.
  const ReflectionProbeRequest &captured = chosenSlot->capturedFor;
  IblSelection selection{};
  selection.prefiltered = chosenSlot->environment.prefilteredTexture;
  selection.irradiance = chosenSlot->environment.irradianceTexture;
  selection.prefilteredMipLevels = chosenSlot->environment.prefilteredMipLevels;
  selection.available = true;
  selection.probeBoxMin =
      math::Vec4(captured.boxMin.x, captured.boxMin.y, captured.boxMin.z,
                 captured.boxProjection ? 1.0F : 0.0F);
  selection.probeBoxMax = math::Vec4(captured.boxMax.x, captured.boxMax.y,
                                     captured.boxMax.z, captured.intensity);
  selection.probeCenter = math::Vec4(captured.position.x, captured.position.y,
                                     captured.position.z, 0.0F);
  return selection;
}

void destroy_reflection_probe_resources(BackendState &backend) noexcept {
  for (ReflectionProbeSlot &slot : backend.reflectionProbes) {
    release_slot(slot);
  }
}

} // namespace engine::renderer
