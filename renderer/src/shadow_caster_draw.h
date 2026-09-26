// Declares the shadow-caster draw the directional, spot and point shadow
// passes share: choosing the program each caster needs (opaque, skinned,
// alpha-masked, skinned and masked), uploading its transforms and mask, and
// drawing it.

#pragma once

#include <cstdint>

#include "engine/math/mat4.h"
#include "engine/math/vec3.h"
#include "engine/renderer/render_device.h"

namespace engine::renderer {

struct BackendState;
struct DrawCommand;
struct GpuMesh;

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

} // namespace engine::renderer
