#pragma once

#include "engine/math/vec3.h"

namespace engine::renderer {

struct Material final {
  math::Vec3 albedo = math::Vec3(1.0F, 1.0F, 1.0F);
  float roughness = 0.5F;
  float metallic = 0.0F;
};

} // namespace engine::renderer
