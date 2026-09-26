// Implements the binding of the scene's sky light to the renderer's
// environment cubemap.

#include "scene_environment.h"

#include <cstdio>

#include "engine/content/asset_identity.h"
#include "engine/core/logging.h"
#include "engine/renderer/command_buffer.h"
#include "engine/runtime/world.h"

namespace engine::runtime {

namespace {

constexpr const char *kLogChannel = "assets";

/// Logs an environment the sky light names but the engine cannot use.
void log_environment_error(const core::AssetRef &ref, const char *path,
                           const char *what) noexcept {
  char refText[content::kAssetRefTextLength + 1U] = {};
  if (!content::format_asset_ref(ref, refText, sizeof(refText))) {
    refText[0] = '\0';
  }
  char message[512] = {};
  std::snprintf(message, sizeof(message),
                "sky light environment %s %s; the scene has no environment "
                "light: %s",
                refText, what, (path != nullptr) ? path : "(no path)");
  core::log_message(core::LogLevel::Error, kLogChannel, message);
}

} // namespace

void release_scene_environment(SceneEnvironment *state,
                               EnvironmentReleaseFn release,
                               void *userData) noexcept {
  if (state == nullptr) {
    return;
  }
  if (state->cubemap != renderer::kInvalidTextureHandle) {
    if (renderer::get_skybox_texture() == state->cubemap) {
      renderer::set_skybox_texture(renderer::kInvalidTextureHandle);
    }
    if (release != nullptr) {
      release(state->cubemap, userData);
    }
  }
  *state = SceneEnvironment{};
}

void update_scene_environment(World *world, const content::AssetCatalog *catalog,
                              SceneEnvironment *state, EnvironmentLoadFn load,
                              EnvironmentReleaseFn release,
                              void *userData) noexcept {
  if ((world == nullptr) || (state == nullptr)) {
    return;
  }
  SkyLightComponent *skyLight =
      (world->sky_light_count() > 0U)
          ? world->get_sky_light_component_ptr(world->sky_light_entity_at(0U))
          : nullptr;
  const core::AssetRef wanted =
      (skyLight != nullptr) ? skyLight->environmentRef : core::AssetRef{};
  const std::uint64_t generation =
      (catalog != nullptr) ? catalog->generation : 0U;

  const bool settled =
      (state->cubemap != renderer::kInvalidTextureHandle) ||
      !core::asset_ref_is_valid(wanted) ||
      (state->catalogGeneration == generation);
  if (state->bound && (wanted == state->ref) && settled) {
    if ((skyLight != nullptr) &&
        (skyLight->environmentAssetId != state->assetId)) {
      skyLight->environmentAssetId = state->assetId;
    }
    return;
  }

  release_scene_environment(state, release, userData);
  state->ref = wanted;
  state->bound = true;
  state->catalogGeneration = generation;
  if (skyLight != nullptr) {
    skyLight->environmentAssetId = content::kInvalidAssetId;
  }
  if (!core::asset_ref_is_valid(wanted)) {
    return;
  }

  const content::AssetMetadata *metadata =
      content::find_asset_metadata_by_ref(catalog, wanted);
  if ((metadata == nullptr) || (metadata->filePath[0] == '\0')) {
    log_environment_error(wanted, nullptr, "is not a catalogued asset");
    return;
  }
  if (metadata->typeTag != content::AssetTypeTag::Environment) {
    log_environment_error(wanted, metadata->filePath.data(),
                          "is not an environment map (.hdr)");
    return;
  }
  state->assetId = metadata->assetId;
  if (skyLight != nullptr) {
    skyLight->environmentAssetId = metadata->assetId;
  }

  const renderer::TextureHandle cubemap =
      (load != nullptr) ? load(metadata->filePath.data(), userData)
                        : renderer::kInvalidTextureHandle;
  if (cubemap == renderer::kInvalidTextureHandle) {
    log_environment_error(wanted, metadata->filePath.data(),
                          "failed to load");
    return;
  }
  state->cubemap = cubemap;
  renderer::set_skybox_texture(cubemap);
}

} // namespace engine::runtime
