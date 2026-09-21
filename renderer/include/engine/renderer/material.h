// Declares material types and APIs for the Engine renderer system.

#pragma once

#include <cstddef>
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

/// How a draw's surface is lit. This is the material's own property, not a
/// global mode: an anime-styled scene puts a toon character, an unlit
/// effect quad and a physically-lit prop in one frame, so the choice
/// belongs to each material and the passes are partitioned per draw.
///
/// The values are the sort key's shader field, so a draw's shading model
/// decides which contiguous run it lands in and the flush binds one
/// program per run. Pbr is 0 so a default-constructed material keeps the
/// behavior every existing draw had.
enum class ShadingModel : std::uint8_t { Pbr = 0U, Toon = 1U, Unlit = 2U };

/// The count of shading models, for the run tables the flush partitions
/// draws into. A new enumerator without a run is a compile error there.
inline constexpr std::size_t kShadingModelCount = 3U;

/// Whether `value` names a shading model this build knows.
constexpr bool shading_model_is_valid(std::uint8_t value) noexcept {
  return value < static_cast<std::uint8_t>(kShadingModelCount);
}

/// PBR constants for a draw (albedo, roughness, metallic, opacity) plus the
/// resolved GPU texture handles a material asset may bind. Handles are
/// resolved once at material load/refresh (never per frame, never on a hot
/// path) by resolve_material_textures; kInvalidTextureHandle means "no
/// texture for this slot" (authoring omitted it, or it failed to load and
/// the material falls back to its scalar parameters). normalTexture stays
/// reserved: tangent-space vectors are not part of the vertex format yet,
/// so no pass samples it. A texture slot a shader silently ignores is
/// never exposed.
struct Material final {
  math::Vec3 albedo = math::Vec3(1.0F, 1.0F, 1.0F);
  math::Vec3 emissive = math::Vec3(0.0F, 0.0F, 0.0F);
  float roughness = 0.5F;
  float metallic = 0.0F;
  float opacity = 1.0F;
  ShadingModel shadingModel = ShadingModel::Pbr;
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
