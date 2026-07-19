// Implements the editor's undoable edit commands and component snapshot
// helpers. Split out of editor.cpp (REVIEW_FINDINGS A3).

#include "editor_commands.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#if __has_include(<SDL.h>)
#include <SDL.h>
#elif __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#error "SDL2 headers not found"
#endif

#if __has_include(<SDL_opengl.h>)
#include <SDL_opengl.h>
#elif __has_include(<SDL2/SDL_opengl.h>)
#include <SDL2/SDL_opengl.h>
#else
#error "SDL OpenGL headers not found"
#endif

#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <vector>

#include "engine/core/cvar.h"
#include "engine/core/engine_stats.h"
#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/core/mem_tracker.h"
#include "engine/core/profiler.h"
#include "engine/core/reflect.h"
#include "engine/engine.h"
#include "engine/editor/editor_camera.h"
#include "engine/math/transform.h"
#include "engine/math/vec2.h"
#include "engine/math/vec4.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/command_buffer.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include "ImGuizmo.h"

#include "engine/editor/command_history.h"
#include "engine/editor/debug_camera.h"

#include <stb_image.h>

namespace engine::editor {

bool capture_component_snapshot(ComponentEditType type, runtime::Entity entity,
                                ComponentEditSnapshot *out) noexcept {
  if ((editor_session().world == nullptr) || (out == nullptr)) {
    return false;
  }

  switch (type) {
  case ComponentEditType::Name:
    return editor_session().world->get_name_component(entity, &out->name);
  case ComponentEditType::Transform:
    return editor_session().world->get_transform(entity, &out->transform);
  case ComponentEditType::RigidBody:
    return editor_session().world->get_rigid_body(entity, &out->rigidBody);
  case ComponentEditType::Collider:
    return editor_session().world->get_collider(entity, &out->collider);
  case ComponentEditType::Light:
    return editor_session().world->get_light_component(entity, &out->light);
  case ComponentEditType::Mesh:
    return editor_session().world->get_mesh_component(entity, &out->mesh);
  case ComponentEditType::FoliagePatch:
    return editor_session().world->get_foliage_patch_component(entity, &out->foliagePatch);
  case ComponentEditType::Script:
    return editor_session().world->get_script_component(entity, &out->script);
  case ComponentEditType::ReflectionProbe:
    return editor_session().world->get_reflection_probe_component(entity,
                                                   &out->reflectionProbe);
  case ComponentEditType::PointLight:
    return editor_session().world->get_point_light_component(entity, &out->pointLight);
  case ComponentEditType::SpotLight:
    return editor_session().world->get_spot_light_component(entity, &out->spotLight);
  case ComponentEditType::SpringArm:
    return editor_session().world->get_spring_arm(entity, &out->springArm);
  case ComponentEditType::SceneCapture:
    return editor_session().world->get_scene_capture_component(
        entity, &out->sceneCapture);
  }
  return false;
}


bool apply_component_snapshot(ComponentEditType type, runtime::Entity entity,
                              bool exists,
                              const ComponentEditSnapshot &snapshot) noexcept {
  if (editor_session().world == nullptr) {
    return false;
  }
  const runtime::Entity resolved =
      editor_session().world->find_entity_by_index(entity.index);
  if (resolved == runtime::kInvalidEntity) {
    return false;
  }

  if (!exists) {
    switch (type) {
    case ComponentEditType::Name:
      return editor_session().world->remove_name_component(resolved);
    case ComponentEditType::Transform:
      return editor_session().world->remove_transform(resolved);
    case ComponentEditType::RigidBody:
      return editor_session().world->remove_rigid_body(resolved);
    case ComponentEditType::Collider:
      return editor_session().world->remove_collider(resolved);
    case ComponentEditType::Light:
      return editor_session().world->remove_light_component(resolved);
    case ComponentEditType::Mesh:
      return editor_session().world->remove_mesh_component(resolved);
    case ComponentEditType::FoliagePatch:
      return editor_session().world->remove_foliage_patch_component(resolved);
    case ComponentEditType::Script:
      return editor_session().world->remove_script_component(resolved);
    case ComponentEditType::ReflectionProbe:
      return editor_session().world->remove_reflection_probe_component(resolved);
    case ComponentEditType::PointLight:
      return editor_session().world->remove_point_light_component(resolved);
    case ComponentEditType::SpotLight:
      return editor_session().world->remove_spot_light_component(resolved);
    case ComponentEditType::SpringArm:
      return editor_session().world->remove_spring_arm(resolved);
    case ComponentEditType::SceneCapture:
      return editor_session().world->remove_scene_capture_component(resolved);
    }
    return false;
  }

  switch (type) {
  case ComponentEditType::Name:
    return editor_session().world->add_name_component(resolved, snapshot.name);
  case ComponentEditType::Transform:
    return editor_session().world->add_transform(resolved, snapshot.transform);
  case ComponentEditType::RigidBody:
    return editor_session().world->add_rigid_body(resolved, snapshot.rigidBody);
  case ComponentEditType::Collider:
    return editor_session().world->add_collider(resolved, snapshot.collider);
  case ComponentEditType::Light:
    return editor_session().world->add_light_component(resolved, snapshot.light);
  case ComponentEditType::Mesh:
    return editor_session().world->add_mesh_component(resolved, snapshot.mesh);
  case ComponentEditType::FoliagePatch:
    return editor_session().world->add_foliage_patch_component(resolved,
                                                snapshot.foliagePatch);
  case ComponentEditType::Script:
    return editor_session().world->add_script_component(resolved, snapshot.script);
  case ComponentEditType::ReflectionProbe:
    return editor_session().world->add_reflection_probe_component(resolved,
                                                   snapshot.reflectionProbe);
  case ComponentEditType::PointLight:
    return editor_session().world->add_point_light_component(resolved, snapshot.pointLight);
  case ComponentEditType::SpotLight:
    return editor_session().world->add_spot_light_component(resolved, snapshot.spotLight);
  case ComponentEditType::SpringArm:
    return editor_session().world->add_spring_arm(resolved, snapshot.springArm);
  case ComponentEditType::SceneCapture:
    return editor_session().world->add_scene_capture_component(
        resolved, snapshot.sceneCapture);
  }
  return false;
}


void execute_component_add(runtime::Entity entity, ComponentEditType type,
                           const ComponentEditSnapshot &after) noexcept {
  ComponentEditSnapshot before{};
  const bool beforeExists = capture_component_snapshot(type, entity, &before);

  auto *cmd = new (std::nothrow) ComponentEditCommand();
  if (cmd == nullptr) {
    static_cast<void>(apply_component_snapshot(type, entity, true, after));
    return;
  }

  cmd->entity = entity;
  cmd->type = type;
  cmd->beforeExists = beforeExists;
  cmd->before = before;
  cmd->afterExists = true;
  cmd->after = after;
  editor_session().commandHistory.execute(cmd);
}


void execute_component_remove(runtime::Entity entity,
                              ComponentEditType type) noexcept {
  ComponentEditSnapshot before{};
  if (!capture_component_snapshot(type, entity, &before)) {
    return;
  }

  auto *cmd = new (std::nothrow) ComponentEditCommand();
  if (cmd == nullptr) {
    static_cast<void>(apply_component_snapshot(type, entity, false, before));
    return;
  }

  cmd->entity = entity;
  cmd->type = type;
  cmd->beforeExists = true;
  cmd->before = before;
  cmd->afterExists = false;
  editor_session().commandHistory.execute(cmd);
}


ComponentEditSnapshot default_component_snapshot(
    runtime::Entity entity, ComponentEditType type) noexcept {
  ComponentEditSnapshot snapshot{};
  switch (type) {
  case ComponentEditType::Name:
    make_default_entity_name(entity.index, &snapshot.name);
    break;
  case ComponentEditType::RigidBody:
    snapshot.rigidBody.inverseMass = 1.0F;
    break;
  case ComponentEditType::Collider:
    snapshot.collider.halfExtents = math::Vec3(0.5F, 0.5F, 0.5F);
    break;
  case ComponentEditType::Mesh:
    snapshot.mesh.albedo = math::Vec3(1.0F, 1.0F, 1.0F);
    break;
  case ComponentEditType::FoliagePatch: {
    snapshot.foliagePatch.instanceCount = 16U;
    snapshot.foliagePatch.density = 1.0F;
    snapshot.foliagePatch.albedo = math::Vec3(0.22F, 0.62F, 0.24F);
    runtime::MeshComponent sourceMesh{};
    if ((editor_session().world != nullptr) &&
        editor_session().world->get_mesh_component(entity, &sourceMesh)) {
      snapshot.foliagePatch.meshAssetIds[0] = sourceMesh.meshAssetId;
      snapshot.foliagePatch.meshAssetIds[1] = sourceMesh.meshAssetId;
    }
    for (std::uint32_t i = 0U; i < snapshot.foliagePatch.instanceCount; ++i) {
      const std::uint32_t x = i % 4U;
      const std::uint32_t z = i / 4U;
      runtime::FoliageInstance &instance =
          snapshot.foliagePatch.instances[i];
      instance.offset = math::Vec3((static_cast<float>(x) - 1.5F) * 0.9F,
                                   0.0F,
                                   (static_cast<float>(z) - 1.5F) * 0.9F);
      instance.scale = 0.55F + (static_cast<float>(i % 3U) * 0.08F);
      instance.phase = static_cast<float>(i) * 0.41F;
      instance.lodIndex = (i >= 12U) ? 1U : 0U;
    }
    break;
  }
  case ComponentEditType::Transform:
  case ComponentEditType::Light:
  case ComponentEditType::Script:
  case ComponentEditType::ReflectionProbe:
  case ComponentEditType::PointLight:
  case ComponentEditType::SpotLight:
  case ComponentEditType::SpringArm:
  case ComponentEditType::SceneCapture:
    break;
  }
  return snapshot;
}


void make_default_entity_name(std::uint32_t entityIndex,
                              runtime::NameComponent *outName) noexcept {
  if (outName == nullptr) {
    return;
  }

  std::snprintf(outName->name, sizeof(outName->name), "Entity_%u", entityIndex);
  outName->name[sizeof(outName->name) - 1U] = '\0';
}


} // namespace engine::editor
