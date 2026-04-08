#pragma once

#include <cstdint>

namespace engine::runtime {

class World;

} // namespace engine::runtime

namespace engine::scripting {

struct RuntimeRaycastHit final {
  std::uint32_t entityIndex = 0U;
  float distance = 0.0F;
  float pointX = 0.0F;
  float pointY = 0.0F;
  float pointZ = 0.0F;
  float normalX = 0.0F;
  float normalY = 0.0F;
  float normalZ = 0.0F;
};

struct RuntimeServices final {
  void (*set_camera_position)(float x, float y, float z) noexcept = nullptr;
  void (*set_camera_target)(float x, float y, float z) noexcept = nullptr;
  void (*set_camera_up)(float x, float y, float z) noexcept = nullptr;
  void (*set_camera_fov)(float fovRadians) noexcept = nullptr;

  void (*set_gravity)(runtime::World *world, float x, float y,
                      float z) noexcept = nullptr;
  bool (*get_gravity)(runtime::World *world, float *outX, float *outY,
                      float *outZ) noexcept = nullptr;
  bool (*raycast)(runtime::World *world, float ox, float oy, float oz, float dx,
                  float dy, float dz, float maxDistance,
                  RuntimeRaycastHit *outHit) noexcept = nullptr;
  std::uint32_t (*add_distance_joint)(runtime::World *world,
                                      std::uint32_t entityIndexA,
                                      std::uint32_t entityIndexB,
                                      float distance) noexcept = nullptr;
  void (*remove_joint)(runtime::World *world,
                       std::uint32_t jointId) noexcept = nullptr;
  void (*wake_body)(runtime::World *world,
                    std::uint32_t entityIndex) noexcept = nullptr;
  bool (*is_sleeping)(runtime::World *world,
                      std::uint32_t entityIndex) noexcept = nullptr;

  std::uint32_t (*load_sound)(const char *path) noexcept = nullptr;
  void (*unload_sound)(std::uint32_t soundId) noexcept = nullptr;
  bool (*play_sound)(std::uint32_t soundId, float volume, float pitch,
                     bool loop) noexcept = nullptr;
  void (*stop_sound)(std::uint32_t soundId) noexcept = nullptr;
  void (*stop_all_sounds)() noexcept = nullptr;
  void (*set_master_volume)(float volume) noexcept = nullptr;

  bool (*save_scene)(const runtime::World *world,
                     const char *path) noexcept = nullptr;
  bool (*save_prefab)(const runtime::World *world, std::uint32_t entityIndex,
                      const char *path) noexcept = nullptr;
  std::uint32_t (*instantiate_prefab)(runtime::World *world,
                                      const char *path) noexcept = nullptr;
};

void bind_runtime_world(runtime::World *world) noexcept;
void bind_runtime_services(const RuntimeServices *services) noexcept;

} // namespace engine::scripting

namespace engine::runtime {

void bind_scripting_runtime(World *world) noexcept;

} // namespace engine::runtime