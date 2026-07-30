// Implements per-frame scene data collection for the pipeline: light and
// scene-capture gathering for the renderer plus the diagnostics counters.

#include "engine_frame_collect.h"

#include <cstddef>

#include "engine/runtime/world.h"
#include "spatial_transform_util.h"

namespace engine {

bool vec3_has_motion(const math::Vec3 &value) noexcept {
  constexpr float kEpsilon = 0.0001F;
  return (value.x > kEpsilon) || (value.x < -kEpsilon) ||
         (value.y > kEpsilon) || (value.y < -kEpsilon) ||
         (value.z > kEpsilon) || (value.z < -kEpsilon);
}

std::size_t count_moving_rigid_bodies(const runtime::World &world) noexcept {
  std::size_t count = 0U;
  world.for_each<runtime::RigidBody>(
      [&count](runtime::Entity, const runtime::RigidBody &rigidBody) noexcept {
        if (vec3_has_motion(rigidBody.velocity) ||
            vec3_has_motion(rigidBody.acceleration)) {
          ++count;
        }
      });
  return count;
}

std::size_t count_mesh_components(const runtime::World &world) noexcept {
  std::size_t count = 0U;
  world.for_each<runtime::MeshComponent>(
      [&count](runtime::Entity, const runtime::MeshComponent &) noexcept {
        ++count;
      });
  return count;
}

std::size_t
count_ready_mesh_components(const runtime::World &world,
                            const renderer::AssetDatabase *assets) noexcept {
  if (assets == nullptr) {
    return 0U;
  }

  std::size_t count = 0U;
  world.for_each<runtime::MeshComponent>(
      [&count, assets](runtime::Entity,
                       const runtime::MeshComponent &mesh) noexcept {
        if (renderer::mesh_asset_state(assets, mesh.meshAssetId) ==
            renderer::AssetState::Ready) {
          ++count;
        }
      });
  return count;
}


MeshAssetStateCounts
count_mesh_asset_states(const renderer::AssetDatabase *assets) noexcept {
  MeshAssetStateCounts counts{};
  if (assets == nullptr) {
    return counts;
  }

  for (std::size_t i = 0U; i < assets->meshAssets.size(); ++i) {
    if (!assets->occupied[i]) {
      continue;
    }

    switch (assets->meshAssets[i].state) {
    case renderer::AssetState::Ready:
      ++counts.ready;
      break;
    case renderer::AssetState::Loading:
      ++counts.loading;
      break;
    case renderer::AssetState::Failed:
      ++counts.failed;
      break;
    case renderer::AssetState::Unloaded:
      break;
    }
  }

  return counts;
}

renderer::SceneLightData
collect_scene_lights(const runtime::World &world) noexcept {
  renderer::SceneLightData sceneLights{};

  const std::size_t lightCount = world.light_count();
  for (std::size_t li = 0U; li < lightCount; ++li) {
    const runtime::LightComponent *lc = world.light_at(li);
    if (lc == nullptr) {
      continue;
    }
    if (lc->type == runtime::LightType::Directional) {
      if (sceneLights.directionalLightCount < renderer::kMaxDirectionalLights) {
        const runtime::Entity lightEntity = world.light_entity_at(li);
        const runtime::WorldTransform *wt =
            world.get_world_transform_read_ptr(lightEntity);
        auto &dl =
            sceneLights.directionalLights[sceneLights.directionalLightCount];
        dl.direction = (wt != nullptr)
                           ? runtime::detail::rotate_local_direction(
                                 wt->rotation, lc->direction)
                           : lc->direction;
        dl.color = lc->color;
        dl.intensity = lc->intensity;
        ++sceneLights.directionalLightCount;
      }
    } else if (lc->type == runtime::LightType::Point) {
      if (sceneLights.pointLightCount < renderer::kMaxPointLights) {
        const runtime::Entity ple = world.light_entity_at(li);
        const runtime::WorldTransform *wt =
            world.get_world_transform_read_ptr(ple);
        auto &pl = sceneLights.pointLights[sceneLights.pointLightCount];
        pl.position =
            (wt != nullptr) ? wt->position : math::Vec3(0.0F, 0.0F, 0.0F);
        pl.color = lc->color;
        pl.intensity = lc->intensity;
        ++sceneLights.pointLightCount;
      }
    }
  }

  const std::size_t plcCount = world.point_light_count();
  for (std::size_t pi = 0U; pi < plcCount; ++pi) {
    if (sceneLights.pointLightCount >= renderer::kMaxPointLights) {
      break;
    }
    const runtime::PointLightComponent *plc = world.point_light_at(pi);
    if (plc == nullptr) {
      continue;
    }
    const runtime::Entity plEntity = world.point_light_entity_at(pi);
    const runtime::WorldTransform *wt =
        world.get_world_transform_read_ptr(plEntity);
    auto &pl = sceneLights.pointLights[sceneLights.pointLightCount];
    pl.position = (wt != nullptr) ? wt->position : math::Vec3(0.0F, 0.0F, 0.0F);
    pl.color = plc->color;
    pl.intensity = plc->intensity;
    pl.radius = plc->radius;
    ++sceneLights.pointLightCount;
  }

  const std::size_t slcCount = world.spot_light_count();
  for (std::size_t si = 0U; si < slcCount; ++si) {
    if (sceneLights.spotLightCount >= renderer::kMaxSpotLights) {
      break;
    }
    const runtime::SpotLightComponent *slc = world.spot_light_at(si);
    if (slc == nullptr) {
      continue;
    }
    const runtime::Entity slEntity = world.spot_light_entity_at(si);
    const runtime::WorldTransform *wt =
        world.get_world_transform_read_ptr(slEntity);
    auto &sl = sceneLights.spotLights[sceneLights.spotLightCount];
    sl.position = (wt != nullptr) ? wt->position : math::Vec3(0.0F, 0.0F, 0.0F);
    sl.direction = (wt != nullptr) ? runtime::detail::rotate_local_direction(
                                         wt->rotation, slc->direction)
                                   : slc->direction;
    sl.color = slc->color;
    sl.intensity = slc->intensity;
    sl.radius = slc->radius;
    sl.innerConeAngle = slc->innerConeAngle;
    sl.outerConeAngle = slc->outerConeAngle;
    ++sceneLights.spotLightCount;
  }

  return sceneLights;
}

// ---------------------------------------------------------------------------
// Scene capture collection
// ---------------------------------------------------------------------------

// Converts enabled SceneCaptureComponents (dense order) into renderer capture
// requests; the capture camera looks along the world rotation's -Z axis.
std::size_t collect_scene_captures(const runtime::World &world,
                                   renderer::SceneCaptureRequest *outRequests,
                                   std::size_t capacity) noexcept {
  if (outRequests == nullptr) {
    return 0U;
  }

  std::size_t requestCount = 0U;
  const std::size_t captureCount = world.scene_capture_count();
  for (std::size_t ci = 0U; (ci < captureCount) && (requestCount < capacity);
       ++ci) {
    const runtime::SceneCaptureComponent *capture = world.scene_capture_at(ci);
    if ((capture == nullptr) || !capture->enabled) {
      continue;
    }

    const runtime::Entity captureEntity = world.scene_capture_entity_at(ci);
    const runtime::WorldTransform *wt =
        world.get_world_transform_read_ptr(captureEntity);
    const math::Vec3 position =
        (wt != nullptr) ? wt->position : math::Vec3(0.0F, 0.0F, 0.0F);
    const math::Quat rotation = (wt != nullptr) ? wt->rotation : math::Quat();

    renderer::SceneCaptureRequest &request = outRequests[requestCount];
    request = renderer::SceneCaptureRequest{};
    request.camera.position = position;
    request.camera.target = math::add(
        position, math::rotate_vector(math::Vec3(0.0F, 0.0F, -1.0F), rotation));
    request.camera.up =
        math::rotate_vector(math::Vec3(0.0F, 1.0F, 0.0F), rotation);
    request.camera.fovRadians = capture->fovRadians;
    request.camera.nearPlane = capture->nearPlane;
    request.camera.farPlane = capture->farPlane;
    request.width = capture->width;
    request.height = capture->height;
    ++requestCount;
  }

  return requestCount;
}


} // namespace engine
