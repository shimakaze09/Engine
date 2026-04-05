#include "engine/editor/editor.h"

#if defined(__clang__) && !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H
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

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/core/reflect.h"
#include "engine/math/vec2.h"
#include "engine/math/vec4.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

namespace engine::editor {

namespace {

bool g_editorInitialized = false;
runtime::World *g_world = nullptr;
std::uint32_t g_selectedEntityIndex = 0U;
enum class PlayState : std::uint8_t { Stopped, Playing, Paused };
PlayState g_playState = PlayState::Stopped;
std::unique_ptr<char[]> g_playSnapshotBuffer{};
std::size_t g_playSnapshotCapacity = 0U;
std::size_t g_playSnapshotSize = 0U;
bool g_hasPlaySnapshot = false;
bool g_worldRestoreFailed = false;
constexpr const char *kTransformTypeName = "engine::runtime::Transform";
constexpr const char *kRigidBodyTypeName = "engine::runtime::RigidBody";
constexpr const char *kColliderTypeName = "engine::runtime::Collider";
constexpr const char *kNameTypeName = "engine::runtime::NameComponent";
constexpr const char *kTransformSectionLabel = "Transform";
constexpr const char *kRigidBodySectionLabel = "RigidBody";
constexpr const char *kColliderSectionLabel = "Collider";
constexpr const char *kMeshSectionLabel = "MeshComponent";
constexpr const char *kScenePath = "assets/scene.json";

bool world_is_editable() noexcept {
  return (g_world != nullptr) && !g_worldRestoreFailed
         && (g_playState == PlayState::Stopped)
         && (g_world->current_phase() == runtime::WorldPhase::Idle);
}

bool world_can_load_scene() noexcept {
  return (g_world != nullptr)
         && (g_world->current_phase() == runtime::WorldPhase::Idle);
}

void make_default_entity_name(std::uint32_t entityIndex,
                              runtime::NameComponent *outName) noexcept {
  if (outName == nullptr) {
    return;
  }

  std::snprintf(outName->name, sizeof(outName->name), "Entity_%u", entityIndex);
  outName->name[sizeof(outName->name) - 1U] = '\0';
}

bool capture_play_snapshot() noexcept {
  if (g_world == nullptr) {
    return false;
  }

  std::size_t capacity = g_playSnapshotCapacity;
  if (capacity < core::JsonWriter::kBufferBytes) {
    capacity = core::JsonWriter::kBufferBytes;
  }

  const std::size_t estimatedCapacity =
      (g_world->alive_entity_count() * 256U) + 4096U;
  if (capacity < estimatedCapacity) {
    capacity = estimatedCapacity;
  }

  for (std::size_t attempt = 0U; attempt < 6U; ++attempt) {
    std::unique_ptr<char[]> candidate(new (std::nothrow) char[capacity]);
    if (candidate == nullptr) {
      return false;
    }

    std::size_t snapshotSize = 0U;
    if (runtime::save_scene(
            *g_world, candidate.get(), capacity, &snapshotSize)) {
      g_playSnapshotBuffer.swap(candidate);
      g_playSnapshotCapacity = capacity;
      g_playSnapshotSize = snapshotSize;
      g_hasPlaySnapshot = true;
      return true;
    }

    if (capacity >= core::JsonWriter::kMaxBufferBytes) {
      break;
    }

    const std::size_t doubledCapacity = capacity * 2U;
    if ((doubledCapacity <= capacity)
        || (doubledCapacity > core::JsonWriter::kMaxBufferBytes)) {
      capacity = core::JsonWriter::kMaxBufferBytes;
    } else {
      capacity = doubledCapacity;
    }
  }

  return false;
}

void start_play_mode() noexcept {
  if (g_world == nullptr) {
    return;
  }

  if (g_worldRestoreFailed) {
    core::log_message(core::LogLevel::Warning,
                      "editor",
                      "play blocked: load scene to recover from restore error");
    return;
  }

  if (g_playState == PlayState::Playing) {
    return;
  }

  if (g_playState == PlayState::Stopped) {
    if (!capture_play_snapshot()) {
      core::log_message(core::LogLevel::Error,
                        "editor",
                        "failed to capture pre-play scene snapshot");
      return;
    }
  }

  g_playState = PlayState::Playing;
  core::log_message(core::LogLevel::Info, "editor", "play");
}

void pause_play_mode() noexcept {
  if ((g_world == nullptr) || (g_playState != PlayState::Playing)) {
    return;
  }

  g_playState = PlayState::Paused;
  core::log_message(core::LogLevel::Info, "editor", "pause");
}

void stop_play_mode() noexcept {
  if ((g_world == nullptr) || (g_playState == PlayState::Stopped)) {
    return;
  }

  bool restored = true;

  if (!g_hasPlaySnapshot || (g_playSnapshotSize == 0U)) {
    core::log_message(core::LogLevel::Warning,
                      "editor",
                      "stop requested without pre-play snapshot");
    restored = false;
  } else if (!runtime::load_scene(
                 *g_world, g_playSnapshotBuffer.get(), g_playSnapshotSize)) {
    core::log_message(core::LogLevel::Error,
                      "editor",
                      "failed to restore pre-play scene snapshot");
    runtime::reset_world(*g_world);
    core::log_message(core::LogLevel::Warning,
                      "editor",
                      "world reset to empty after restore failure");
    g_selectedEntityIndex = 0U;
    // restored stays true: world is clean and usable, just empty
  } else {
    g_selectedEntityIndex = 0U;
  }

  g_playState = PlayState::Stopped;
  g_worldRestoreFailed = !restored;

  core::log_message(core::LogLevel::Info, "editor", "stop");
}

void mark_modified(bool *modified, bool changed) noexcept {
  if ((modified != nullptr) && changed) {
    *modified = true;
  }
}

void draw_vec2_field(const char *label,
                     math::Vec2 &value,
                     bool *modified) noexcept {
  constexpr ImGuiInputTextFlags kCommitFlags =
      ImGuiInputTextFlags_EnterReturnsTrue;

  ImGui::PushID(label);
  ImGui::TextUnformatted(label);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(80.0F);
  mark_modified(
      modified,
      ImGui::InputFloat("##x", &value.x, 0.0F, 0.0F, "%.3f", kCommitFlags));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(80.0F);
  mark_modified(
      modified,
      ImGui::InputFloat("##y", &value.y, 0.0F, 0.0F, "%.3f", kCommitFlags));
  ImGui::PopID();
}

void draw_vec3_field(const char *label,
                     math::Vec3 &value,
                     bool *modified) noexcept {
  constexpr ImGuiInputTextFlags kCommitFlags =
      ImGuiInputTextFlags_EnterReturnsTrue;

  ImGui::PushID(label);
  ImGui::TextUnformatted(label);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(80.0F);
  mark_modified(
      modified,
      ImGui::InputFloat("##x", &value.x, 0.0F, 0.0F, "%.3f", kCommitFlags));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(80.0F);
  mark_modified(
      modified,
      ImGui::InputFloat("##y", &value.y, 0.0F, 0.0F, "%.3f", kCommitFlags));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(80.0F);
  mark_modified(
      modified,
      ImGui::InputFloat("##z", &value.z, 0.0F, 0.0F, "%.3f", kCommitFlags));
  ImGui::PopID();
}

void draw_vec4_field(const char *label,
                     math::Vec4 &value,
                     bool *modified) noexcept {
  constexpr ImGuiInputTextFlags kCommitFlags =
      ImGuiInputTextFlags_EnterReturnsTrue;

  ImGui::PushID(label);
  ImGui::TextUnformatted(label);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70.0F);
  mark_modified(
      modified,
      ImGui::InputFloat("##x", &value.x, 0.0F, 0.0F, "%.3f", kCommitFlags));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70.0F);
  mark_modified(
      modified,
      ImGui::InputFloat("##y", &value.y, 0.0F, 0.0F, "%.3f", kCommitFlags));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70.0F);
  mark_modified(
      modified,
      ImGui::InputFloat("##z", &value.z, 0.0F, 0.0F, "%.3f", kCommitFlags));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70.0F);
  mark_modified(
      modified,
      ImGui::InputFloat("##w", &value.w, 0.0F, 0.0F, "%.3f", kCommitFlags));
  ImGui::PopID();
}

void draw_quat_field(const char *label,
                     math::Quat &value,
                     bool *modified) noexcept {
  constexpr ImGuiInputTextFlags kCommitFlags =
      ImGuiInputTextFlags_EnterReturnsTrue;

  ImGui::PushID(label);
  ImGui::TextUnformatted(label);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70.0F);
  mark_modified(
      modified,
      ImGui::InputFloat("##x", &value.x, 0.0F, 0.0F, "%.3f", kCommitFlags));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70.0F);
  mark_modified(
      modified,
      ImGui::InputFloat("##y", &value.y, 0.0F, 0.0F, "%.3f", kCommitFlags));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70.0F);
  mark_modified(
      modified,
      ImGui::InputFloat("##z", &value.z, 0.0F, 0.0F, "%.3f", kCommitFlags));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70.0F);
  mark_modified(
      modified,
      ImGui::InputFloat("##w", &value.w, 0.0F, 0.0F, "%.3f", kCommitFlags));
  ImGui::PopID();
}

void draw_field(const core::TypeDescriptor &desc,
                void *instance,
                const core::TypeField &field,
                bool *modified) noexcept {
  if ((instance == nullptr) || (field.name == nullptr)) {
    return;
  }

  constexpr ImGuiInputTextFlags kCommitFlags =
      ImGuiInputTextFlags_EnterReturnsTrue;

  switch (field.kind) {
  case core::TypeField::Kind::Float: {
    float *value = desc.field_ptr<float>(instance, field);
    if (value != nullptr) {
      mark_modified(modified,
                    ImGui::InputFloat(
                        field.name, value, 0.0F, 0.0F, "%.3f", kCommitFlags));
    }
    break;
  }
  case core::TypeField::Kind::Int32: {
    std::int32_t *value = desc.field_ptr<std::int32_t>(instance, field);
    if (value != nullptr) {
      mark_modified(modified,
                    ImGui::InputScalar(field.name,
                                       ImGuiDataType_S32,
                                       value,
                                       nullptr,
                                       nullptr,
                                       "%d",
                                       kCommitFlags));
    }
    break;
  }
  case core::TypeField::Kind::Uint32: {
    std::uint32_t *value = desc.field_ptr<std::uint32_t>(instance, field);
    if (value != nullptr) {
      mark_modified(modified,
                    ImGui::InputScalar(field.name,
                                       ImGuiDataType_U32,
                                       value,
                                       nullptr,
                                       nullptr,
                                       "%u",
                                       kCommitFlags));
    }
    break;
  }
  case core::TypeField::Kind::Bool: {
    bool *value = desc.field_ptr<bool>(instance, field);
    if (value != nullptr) {
      mark_modified(modified, ImGui::Checkbox(field.name, value));
    }
    break;
  }
  case core::TypeField::Kind::Vec2: {
    math::Vec2 *value = desc.field_ptr<math::Vec2>(instance, field);
    if (value != nullptr) {
      draw_vec2_field(field.name, *value, modified);
    }
    break;
  }
  case core::TypeField::Kind::Vec3: {
    math::Vec3 *value = desc.field_ptr<math::Vec3>(instance, field);
    if (value != nullptr) {
      draw_vec3_field(field.name, *value, modified);
    }
    break;
  }
  case core::TypeField::Kind::Vec4: {
    math::Vec4 *value = desc.field_ptr<math::Vec4>(instance, field);
    if (value != nullptr) {
      draw_vec4_field(field.name, *value, modified);
    }
    break;
  }
  case core::TypeField::Kind::Quat: {
    math::Quat *value = desc.field_ptr<math::Quat>(instance, field);
    if (value != nullptr) {
      draw_quat_field(field.name, *value, modified);
    }
    break;
  }
  }
}

bool draw_reflected_component(const char *typeName, void *instance) noexcept {
  if ((typeName == nullptr) || (instance == nullptr)) {
    return false;
  }

  const core::TypeDescriptor *desc =
      core::global_type_registry().find_type(typeName);
  if (desc == nullptr) {
    return false;
  }

  bool modified = false;

  for (std::size_t i = 0U; i < desc->fieldCount; ++i) {
    draw_field(*desc, instance, desc->fields[i], &modified);
  }

  return modified;
}

void draw_main_menu_bar() noexcept {
  if (!ImGui::BeginMainMenuBar()) {
    return;
  }

  if (ImGui::BeginMenu("File")) {
    const bool canSaveScene = world_is_editable();
    const bool canLoadScene = world_can_load_scene();

    if (!canSaveScene) {
      ImGui::BeginDisabled();
    }

    if (ImGui::MenuItem("Save Scene") && canSaveScene) {
      if (!runtime::save_scene(*g_world, kScenePath)) {
        core::log_message(core::LogLevel::Error,
                          "editor",
                          "failed to save scene to assets/scene.json");
      }
    }

    if (!canSaveScene) {
      ImGui::EndDisabled();
    }

    if (!canLoadScene) {
      ImGui::BeginDisabled();
    }

    if (ImGui::MenuItem("Load Scene") && canLoadScene) {
      if (!runtime::load_scene(*g_world, kScenePath)) {
        core::log_message(core::LogLevel::Error,
                          "editor",
                          "failed to load scene from assets/scene.json");
      } else {
        g_selectedEntityIndex = 0U;
        g_worldRestoreFailed = false;
      }
    }

    if (!canLoadScene) {
      ImGui::EndDisabled();
    }

    ImGui::EndMenu();
  }

  ImGui::EndMainMenuBar();
}

bool draw_remove_component_button(const char *id, bool editable) noexcept {
  if (!editable || (id == nullptr)) {
    return false;
  }

  ImGui::SameLine();
  ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - 20.0F);
  ImGui::PushID(id);
  const bool removePressed = ImGui::SmallButton("X");
  ImGui::PopID();
  return removePressed;
}

void draw_add_component_combo(runtime::Entity entity, bool editable) noexcept {
  if (!editable || (g_world == nullptr)) {
    return;
  }

  ImGui::Separator();
  if (!ImGui::BeginCombo("##addcomp", "Add Component...")) {
    return;
  }

  const core::TypeRegistry &registry = core::global_type_registry();
  for (std::size_t i = 0U; i < registry.type_count(); ++i) {
    const core::TypeDescriptor *desc = registry.type_at(i);
    if ((desc == nullptr) || (desc->name == nullptr)) {
      continue;
    }

    if (std::strcmp(desc->name, kNameTypeName) == 0) {
      // NameComponent is intentionally managed via the dedicated + Name/- UI.
      continue;
    }

    if ((std::strcmp(desc->name, kTransformTypeName) == 0)
        && (g_world->get_transform_read_ptr(entity) == nullptr)) {
      if (ImGui::Selectable(kTransformSectionLabel)) {
        static_cast<void>(g_world->add_transform(entity, runtime::Transform{}));
      }
      continue;
    }

    if ((std::strcmp(desc->name, kRigidBodyTypeName) == 0)
        && (g_world->get_rigid_body_ptr(entity) == nullptr)) {
      runtime::RigidBody rigidBody{};
      rigidBody.inverseMass = 1.0F;
      if (ImGui::Selectable(kRigidBodySectionLabel)) {
        static_cast<void>(g_world->add_rigid_body(entity, rigidBody));
      }
      continue;
    }

    if ((std::strcmp(desc->name, kColliderTypeName) == 0)
        && (g_world->get_collider_ptr(entity) == nullptr)) {
      runtime::Collider collider{};
      collider.halfExtents = math::Vec3(0.5F, 0.5F, 0.5F);
      if (ImGui::Selectable(kColliderSectionLabel)) {
        static_cast<void>(g_world->add_collider(entity, collider));
      }
      continue;
    }
  }

  if (g_world->get_mesh_component_ptr(entity) == nullptr) {
    runtime::MeshComponent mesh{};
    mesh.meshAssetId = 0U;
    mesh.material.albedo = math::Vec3(1.0F, 1.0F, 1.0F);
    if (ImGui::Selectable(kMeshSectionLabel)) {
      static_cast<void>(g_world->add_mesh_component(entity, mesh));
    }
  }

  ImGui::EndCombo();
}

void draw_toolbar() noexcept {
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  if (viewport == nullptr) {
    return;
  }

  const float menuBarHeight = ImGui::GetFrameHeight();
  const float toolbarHeight = ImGui::GetFrameHeightWithSpacing();

  ImGui::SetNextWindowPos(
      ImVec2(viewport->Pos.x, viewport->Pos.y + menuBarHeight));
  ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, toolbarHeight));
  ImGui::SetNextWindowViewport(viewport->ID);

  constexpr ImGuiWindowFlags kToolbarFlags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollbar
      | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings;

  if (!ImGui::Begin("##toolbar", nullptr, kToolbarFlags)) {
    ImGui::End();
    return;
  }

  const bool hasWorld = (g_world != nullptr);
  const bool canPlay =
      hasWorld && !g_worldRestoreFailed && (g_playState != PlayState::Playing);
  const bool canPause = hasWorld && (g_playState == PlayState::Playing);
  const bool canStop = hasWorld && (g_playState != PlayState::Stopped);

  if (!canPlay) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button("\xE2\x96\xB6 Play") && canPlay) {
    start_play_mode();
  }
  if (!canPlay) {
    ImGui::EndDisabled();
  }

  ImGui::SameLine();
  if (!canPause) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button("\xE2\x8F\xB8 Pause") && canPause) {
    pause_play_mode();
  }
  if (!canPause) {
    ImGui::EndDisabled();
  }

  ImGui::SameLine();
  if (!canStop) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button("\xE2\x8F\xB9 Stop") && canStop) {
    stop_play_mode();
  }
  if (!canStop) {
    ImGui::EndDisabled();
  }

  ImGui::End();
}

void draw_entities_panel() noexcept {
  if (!ImGui::Begin("Entities")) {
    ImGui::End();
    return;
  }

  if (g_world == nullptr) {
    ImGui::TextUnformatted("No world attached");
    ImGui::End();
    return;
  }

  // Selection mutates g_selectedEntityIndex, which is editor-global state.
  g_world->for_each_alive([](runtime::Entity entity) {
    char label[160] = {};
    runtime::NameComponent name{};
    if (g_world->get_name_component(entity, &name) && (name.name[0] != '\0')) {
      std::snprintf(
          label, sizeof(label), "%s###entity_%u", name.name, entity.index);
    } else {
      std::snprintf(label, sizeof(label), "Entity [%u]", entity.index);
    }

    const bool isSelected = (g_selectedEntityIndex == entity.index);
    if (ImGui::Selectable(label, isSelected)) {
      g_selectedEntityIndex = entity.index;
    }
  });

  ImGui::Separator();
  const bool editable = world_is_editable();
  if (!editable) {
    ImGui::BeginDisabled();
  }

  if (ImGui::Button("Create Entity") && editable) {
    const runtime::Entity newEntity = g_world->create_entity();
    if (newEntity != runtime::kInvalidEntity) {
      runtime::NameComponent nameComponent{};
      make_default_entity_name(newEntity.index, &nameComponent);
      static_cast<void>(g_world->add_name_component(newEntity, nameComponent));
      g_selectedEntityIndex = newEntity.index;
    }
  }

  if (!editable) {
    ImGui::EndDisabled();
  }

  ImGui::End();
}

void draw_inspector_panel() noexcept {
  if (!ImGui::Begin("Inspector")) {
    ImGui::End();
    return;
  }

  if (g_worldRestoreFailed) {
    ImGui::TextColored(ImVec4(1.0F, 0.5F, 0.2F, 1.0F),
                       "Scene restore failed on Stop.");
    ImGui::TextUnformatted("Use File -> Load Scene to recover.");
    ImGui::Separator();
  }

  if ((g_world == nullptr) || (g_selectedEntityIndex == 0U)) {
    ImGui::TextUnformatted("No entity selected");
    ImGui::End();
    return;
  }

  const runtime::Entity entity =
      g_world->find_entity_by_index(g_selectedEntityIndex);
  if (entity == runtime::kInvalidEntity) {
    ImGui::TextUnformatted("Selected entity is no longer alive");
    g_selectedEntityIndex = 0U;
    ImGui::End();
    return;
  }

  const bool editable = world_is_editable();
  runtime::NameComponent nameComponent{};
  const bool hasNameComponent =
      g_world->get_name_component(entity, &nameComponent);
  if (hasNameComponent) {
    bool nameChanged = false;
    bool removeNamePressed = false;
    if (!editable) {
      ImGui::BeginDisabled();
    }

    nameChanged = ImGui::InputText(
        "Name", nameComponent.name, sizeof(nameComponent.name));
    ImGui::SameLine();
    removeNamePressed = ImGui::SmallButton("-");

    if (!editable) {
      ImGui::EndDisabled();
    }

    if (editable && removeNamePressed) {
      static_cast<void>(g_world->remove_name_component(entity));
    } else if (editable && nameChanged) {
      static_cast<void>(g_world->add_name_component(entity, nameComponent));
    }
  } else {
    if (!editable) {
      ImGui::BeginDisabled();
    }

    if (ImGui::SmallButton("+ Name") && editable) {
      runtime::NameComponent newName{};
      make_default_entity_name(entity.index, &newName);
      static_cast<void>(g_world->add_name_component(entity, newName));
    }

    if (!editable) {
      ImGui::EndDisabled();
    }
  }

  ImGui::Separator();

  if (!editable) {
    ImGui::BeginDisabled();
  }

  const bool deletePressed = ImGui::Button("Delete Entity");

  if (!editable) {
    ImGui::EndDisabled();
  }

  if (editable && deletePressed) {
    static_cast<void>(g_world->destroy_entity(entity));
    g_selectedEntityIndex = 0U;
    ImGui::End();
    return;
  }

  ImGui::SameLine();
  ImGui::Text("Entity [%u] gen=%u", entity.index, entity.generation);
  ImGui::Separator();

  runtime::Transform transform{};
  if (g_world->get_transform(entity, &transform)) {
    ImGui::PushID("TransformSection");
    const bool sectionOpen = ImGui::CollapsingHeader(
        kTransformSectionLabel, ImGuiTreeNodeFlags_DefaultOpen);
    const bool removePressed = draw_remove_component_button("remove", editable);

    bool transformModified = false;
    if (sectionOpen) {
      if (editable) {
        transformModified =
            draw_reflected_component(kTransformTypeName, &transform);
      } else {
        ImGui::BeginDisabled();
        static_cast<void>(
            draw_reflected_component(kTransformTypeName, &transform));
        ImGui::EndDisabled();
      }
    }
    ImGui::PopID();

    if (editable && removePressed) {
      static_cast<void>(g_world->remove_transform(entity));
    } else if (editable && transformModified) {
      static_cast<void>(g_world->add_transform(entity, transform));
    }
  } else {
    ImGui::TextUnformatted("Transform: <none>");
  }

  runtime::RigidBody rigidBody{};
  if (g_world->get_rigid_body(entity, &rigidBody)) {
    ImGui::PushID("RigidBodySection");
    const bool sectionOpen = ImGui::CollapsingHeader(
        kRigidBodySectionLabel, ImGuiTreeNodeFlags_DefaultOpen);
    const bool removePressed = draw_remove_component_button("remove", editable);

    bool rigidBodyModified = false;
    if (sectionOpen) {
      if (editable) {
        rigidBodyModified =
            draw_reflected_component(kRigidBodyTypeName, &rigidBody);
      } else {
        ImGui::BeginDisabled();
        static_cast<void>(
            draw_reflected_component(kRigidBodyTypeName, &rigidBody));
        ImGui::EndDisabled();
      }
    }
    ImGui::PopID();

    if (editable && removePressed) {
      static_cast<void>(g_world->remove_rigid_body(entity));
    } else if (editable && rigidBodyModified) {
      static_cast<void>(g_world->add_rigid_body(entity, rigidBody));
    }
  } else {
    ImGui::TextUnformatted("RigidBody: <none>");
  }

  runtime::Collider collider{};
  if (g_world->get_collider(entity, &collider)) {
    ImGui::PushID("ColliderSection");
    const bool sectionOpen = ImGui::CollapsingHeader(
        kColliderSectionLabel, ImGuiTreeNodeFlags_DefaultOpen);
    const bool removePressed = draw_remove_component_button("remove", editable);

    bool colliderModified = false;
    if (sectionOpen) {
      if (editable) {
        colliderModified =
            draw_reflected_component(kColliderTypeName, &collider);
      } else {
        ImGui::BeginDisabled();
        static_cast<void>(
            draw_reflected_component(kColliderTypeName, &collider));
        ImGui::EndDisabled();
      }
    }
    ImGui::PopID();

    if (editable && removePressed) {
      static_cast<void>(g_world->remove_collider(entity));
    } else if (editable && colliderModified) {
      static_cast<void>(g_world->add_collider(entity, collider));
    }
  } else {
    ImGui::TextUnformatted("Collider: <none>");
  }

  runtime::MeshComponent mesh{};
  if (g_world->get_mesh_component(entity, &mesh)) {
    ImGui::PushID("MeshComponentSection");
    const bool sectionOpen = ImGui::CollapsingHeader(
        kMeshSectionLabel, ImGuiTreeNodeFlags_DefaultOpen);
    const bool removePressed = draw_remove_component_button("remove", editable);

    bool meshModified = false;
    if (sectionOpen) {
      ImGui::Text("Mesh Asset ID: %u", mesh.meshAssetId);
      if (editable) {
        meshModified |= ImGui::ColorEdit3("Albedo", &mesh.material.albedo.x);
      } else {
        ImGui::BeginDisabled();
        static_cast<void>(ImGui::ColorEdit3("Albedo", &mesh.material.albedo.x));
        ImGui::EndDisabled();
      }
    }
    ImGui::PopID();

    if (editable && removePressed) {
      static_cast<void>(g_world->remove_mesh_component(entity));
    } else if (editable && meshModified) {
      static_cast<void>(g_world->add_mesh_component(entity, mesh));
    }
  } else {
    ImGui::TextUnformatted("MeshComponent: <none>");
  }

  draw_add_component_combo(entity, editable);

  ImGui::End();
}

void draw_stats_panel(float frameMs, float utilizationPct) noexcept {
  if (!ImGui::Begin("Stats")) {
    ImGui::End();
    return;
  }

  ImGui::Text("Frame: %.3f ms", frameMs);
  ImGui::Text("Job Utilization: %.2f%%", utilizationPct);

  const std::size_t transformCount =
      (g_world != nullptr) ? g_world->transform_count() : 0U;
  const std::size_t entityCount = transformCount;
  ImGui::Text("Entities: %zu", entityCount);
  ImGui::Text("Transforms: %zu", transformCount);

  ImGui::End();
}

void draw_editor_panels(float frameMs, float utilizationPct) noexcept {
  draw_main_menu_bar();
  draw_toolbar();
  draw_entities_panel();
  draw_inspector_panel();
  draw_stats_panel(frameMs, utilizationPct);
}

} // namespace

bool initialize_editor(void *sdlWindow, void *glContext) noexcept {
  if (g_editorInitialized) {
    return true;
  }

  if ((sdlWindow == nullptr) || (glContext == nullptr)) {
    return false;
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

  ImGui::StyleColorsDark();

  if (!ImGui_ImplSDL2_InitForOpenGL(static_cast<SDL_Window *>(sdlWindow),
                                    static_cast<SDL_GLContext>(glContext))) {
    ImGui::DestroyContext();
    return false;
  }

  if (!ImGui_ImplOpenGL3_Init("#version 330 core")) {
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    return false;
  }

  // Meaningful editor testing requires a live OpenGL context.
  g_editorInitialized = true;
  return true;
}

void shutdown_editor() noexcept {
  if (!g_editorInitialized) {
    return;
  }

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();

  g_editorInitialized = false;
  g_world = nullptr;
  g_selectedEntityIndex = 0U;
  g_playState = PlayState::Stopped;
  g_playSnapshotSize = 0U;
  g_hasPlaySnapshot = false;
  g_worldRestoreFailed = false;
}

void editor_new_frame() noexcept {
  if (!g_editorInitialized) {
    return;
  }

  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplSDL2_NewFrame();
  ImGui::NewFrame();
}

void editor_render(float frameMs, float utilizationPct) noexcept {
  if (!g_editorInitialized) {
    return;
  }

#ifndef NDEBUG
  while (glGetError() != GL_NO_ERROR) {
  }
#endif

  draw_editor_panels(frameMs, utilizationPct);
  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void editor_process_event(void *sdlEvent) noexcept {
  if (!g_editorInitialized || (sdlEvent == nullptr)) {
    return;
  }

  ImGui_ImplSDL2_ProcessEvent(static_cast<SDL_Event *>(sdlEvent));
}

void editor_set_world(runtime::World *world) noexcept {
  g_world = world;
  if (world == nullptr) {
    g_selectedEntityIndex = 0U;
    g_playState = PlayState::Stopped;
    g_playSnapshotSize = 0U;
    g_hasPlaySnapshot = false;
    g_worldRestoreFailed = false;
  }
}

bool editor_is_playing() noexcept {
  return g_playState == PlayState::Playing;
}

bool editor_is_paused() noexcept {
  return g_playState == PlayState::Paused;
}

namespace {

bool editor_wants_capture_keyboard() noexcept {
  if (!g_editorInitialized) {
    return false;
  }

  return ImGui::GetIO().WantCaptureKeyboard;
}

bool editor_wants_capture_mouse() noexcept {
  if (!g_editorInitialized) {
    return false;
  }

  return ImGui::GetIO().WantCaptureMouse;
}

const runtime::EditorBridge kRuntimeEditorBridge = {
    &initialize_editor,
    &shutdown_editor,
    &editor_new_frame,
    &editor_render,
    &editor_process_event,
    &editor_set_world,
    &editor_is_playing,
    &editor_is_paused,
    &editor_wants_capture_keyboard,
    &editor_wants_capture_mouse,
};

[[maybe_unused]] const bool kEditorBridgeRegistered = []() noexcept {
  runtime::set_editor_bridge(&kRuntimeEditorBridge);
  return true;
}();

} // namespace

} // namespace engine::editor
