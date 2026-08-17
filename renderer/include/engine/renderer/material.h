// Declares material types and APIs for the Engine renderer system.

#pragma once

#include <cstdint>

#include "engine/math/vec2.h"
#include "engine/math/vec3.h"
#include "engine/renderer/texture_loader.h"

namespace engine::renderer {

/// Alpha handling for a draw. Opaque and Blend match the pre-existing
/// opacity-driven transparency classification (build_draw_sort_key keys off
/// opacity < 1, unchanged by this enum); Mask adds an alpha-tested cutout
/// (fragment discard below alphaCutoff) that v1 materials never had.
enum class AlphaMode : std::uint8_t { Opaque = 0U, Mask = 1U, Blend = 2U };

/// PBR constants for a draw (albedo, roughness, metallic, opacity) plus the
/// resolved GPU texture handles a material asset may bind. Handles are
/// resolved once at material load/refresh (never per frame, never on a hot
/// path) by resolve_material_textures; kInvalidTextureHandle means "no
/// texture for this slot" (authoring omitted it, or it failed to load and
/// the material falls back to its scalar parameters). normalTexture stays
/// reserved: tangent-space vectors are not part of the vertex format yet,
/// so no pass samples it (issue #160 cut line: never expose a texture slot
/// a shader silently ignores).
struct Material final {
  math::Vec3 albedo = math::Vec3(1.0F, 1.0F, 1.0F);
  math::Vec3 emissive = math::Vec3(0.0F, 0.0F, 0.0F);
  float roughness = 0.5F;
  float metallic = 0.0F;
  float opacity = 1.0F;
  AlphaMode alphaMode = AlphaMode::Opaque;
  float alphaCutoff = 0.5F;
  math::Vec2 uvTiling = math::Vec2(1.0F, 1.0F);
  math::Vec2 uvOffset = math::Vec2(0.0F, 0.0F);

  TextureHandle albedoTexture = kInvalidTextureHandle;
  TextureHandle normalTexture = kInvalidTextureHandle; // reserved, unused
  // Packed per the glTF metallicRoughness convention: G = roughness,
  // B = metallic. One slot instead of two keeps material draws within the
  // free sampler-unit budget shared with shadows/IBL.
  TextureHandle metallicRoughnessTexture = kInvalidTextureHandle;
  TextureHandle emissiveTexture = kInvalidTextureHandle;
  TextureHandle occlusionTexture = kInvalidTextureHandle; // AO, R channel
  TextureHandle opacityTexture = kInvalidTextureHandle;   // mask, R channel
};

} // namespace engine::renderer
