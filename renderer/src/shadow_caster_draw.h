// Declares the shadow-caster draw the directional, spot and point shadow
// passes share: choosing the program each caster needs (opaque, skinned,
// alpha-masked, skinned and masked), uploading its transforms and mask, and
// drawing it; and the directional cache key, which hashes what that draw
// reads so a cached map is reused only while it would redraw the same.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/math/mat4.h"
#include "engine/math/vec3.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/render_device.h"
#include "engine/renderer/shadow_map.h"

namespace engine::renderer {

struct BackendState;
struct GpuMesh;
struct GpuMeshRegistry;

/// The shadow target one caster draws into.
struct ShadowCasterPass final {
  /// Light view-projection of the target. Directional and spot passes fold
  /// the model matrix into it per caster; the point pass uploads the two
  /// apart, since its fragment stage measures the world-space distance.
  math::Mat4 viewProjection = math::Mat4();
  bool point = false;
  /// Point pass only: the light position and far plane the distance is
  /// normalized against.
  math::Vec3 lightPosition = math::Vec3(0.0F, 0.0F, 0.0F);
  float farPlane = 1.0F;
  /// The program the pass keeps bound for plain casters; a caster drawn
  /// through another program rebinds it afterwards.
  DeviceProgramHandle baseProgram = kInvalidDeviceProgram;
};

/// Draws one caster into the bound shadow target with the program it
/// needs: a mask-mode material with an opacity mask casts through the
/// MASKED program (discarding below its cutoff, as the lit passes do), a
/// posed skinned mesh through the skinned one, both through the skinned
/// masked one. An unavailable program falls back to the next that fits,
/// the pose ahead of the mask. Returns the triangles drawn.
std::uint32_t draw_shadow_caster(BackendState &backend, const RenderDevice *dev,
                                 const GpuMesh &mesh,
                                 const DrawCommand &command,
                                 const ShadowCasterPass &pass) noexcept;

/// Key of the directional cascades one frame would draw. Two frames with
/// equal keys draw identical maps, so the pass reuses the cached ones: the
/// key covers the light, the splits and cascade matrices, and per caster
/// its identity and transform and what draw_shadow_caster resolves for it
/// now -- the geometry its mesh handle finds in `registry` and, for a
/// masked caster, the mask texture, cutoff and UV transform. A caster
/// whose geometry or mask finishes loading therefore redraws the maps
/// rather than leaving them as drawn without it.
std::uint64_t directional_shadow_cache_key(
    CommandBufferView commandBufferView, std::size_t opaqueCount,
    const DirectionalLightData &light, const CascadeSplits &splits,
    const std::array<math::Mat4, kShadowCascadeCount> &matrices,
    CommandBufferView auxiliaryView, std::size_t auxiliaryOpaqueCount,
    const GpuMeshRegistry *registry) noexcept;

} // namespace engine::renderer
