#pragma once

#include <cstdint>

#include "engine/math/vec3.h"
#include "engine/renderer/texture_loader.h"

namespace engine::renderer {

struct Material final {
  math::Vec3 albedo = math::Vec3(1.0F, 1.0F, 1.0F);
  math::Vec3 emissive = math::Vec3(0.0F, 0.0F, 0.0F);
  float roughness = 0.5F;
  float metallic = 0.0F;
  float opacity = 1.0F;
  TextureHandle albedoTexture = kInvalidTextureHandle;
  TextureHandle normalTexture = kInvalidTextureHandle; // reserved
};

} // namespace engine::renderer
