// Places prefabs, cooked meshes and models from the Assets panel as one
// undoable step. Every placement is captured into a single duplicate
// record and run through EntityDuplicateCommand, so placing, undoing and
// redoing an instance share the tested create/rollback path of Duplicate
// and Paste rather than a second entity-creation routine.

#include "editor_asset_place.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "editor_component_registry.h"
#include "editor_session.h"
#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/prefab_serializer.h"
#include "imgui.h"

namespace engine::editor {
namespace {

constexpr std::size_t slot_of(ComponentEditType type) noexcept {
  return static_cast<std::size_t>(type);
}

/// Logs why a placement was refused, naming the asset.
void log_refusal(const char *virtualPath, const char *reason) noexcept {
  char message[640] = {};
  std::snprintf(message, sizeof(message), "cannot place '%s': %s",
                (virtualPath != nullptr) ? virtualPath : "", reason);
  core::log_message(core::LogLevel::Warning, "editor", message);
}

/// Copies `path` with its final suffix (from the last '.') replaced by
/// `suffix`; false when the result does not fit whole.
bool replace_suffix(const char *path, const char *suffix, char *out,
                    std::size_t capacity) noexcept {
  const char *dot = std::strrchr(path, '.');
  const char *slash = std::strrchr(path, '/');
  const std::size_t stemLength =
      ((dot != nullptr) && ((slash == nullptr) || (dot > slash)))
          ? static_cast<std::size_t>(dot - path)
          : std::strlen(path);
  const int written = std::snprintf(out, capacity, "%.*s%s",
                                    static_cast<int>(stemLength), path, suffix);
  return (written > 0) && (static_cast<std::size_t>(written) < capacity);
}

/// The folder part of a virtual path ("assets/props/rock.mesh" gives
/// "assets/props"); empty for a bare file name.
void folder_of(const char *path, char *out, std::size_t capacity) noexcept {
  const char *slash = std::strrchr(path, '/');
  const std::size_t length =
      (slash != nullptr) ? static_cast<std::size_t>(slash - path) : 0U;
  std::snprintf(out, capacity, "%.*s", static_cast<int>(length), path);
}

/// True when the controller file at `controllerPath` names `skeletonPath`
/// as its skeleton.
bool controller_drives_skeleton(const char *controllerPath,
                                const char *skeletonPath) noexcept {
  char *text = nullptr;
  std::size_t size = 0U;
  if (!core::vfs_read_text(controllerPath, &text, &size)) {
    return false;
  }
  bool drives = false;
  core::JsonParser parser{};
  if (parser.parse(text, size) && (parser.root() != nullptr)) {
    core::JsonValue value{};
    char skeleton[256] = {};
    drives = parser.get_object_field(*parser.root(), "skeleton", &value) &&
             parser.copy_string_strict(value, skeleton, sizeof(skeleton)) &&
             (std::strcmp(skeleton, skeletonPath) == 0);
  }
  core::vfs_free(text);
  return drives;
}

/// Finds the one animation controller beside a skinned mesh: the mesh's
/// skeleton is `<stem>.skel`, and a controller in the same folder that
/// names it drives it. With none or several, nothing is chosen (several
/// are logged, since picking one would be a guess). Unity's model import
/// likewise adds an Animator but leaves its controller to the author
/// unless one is set; here a lone controller is the author's evident
/// choice.
bool find_controller_for_mesh(const char *meshPath, char *out,
                              std::size_t capacity) noexcept {
  char skeletonPath[512] = {};
  if (!replace_suffix(meshPath, ".skel", skeletonPath, sizeof(skeletonPath)) ||
      !core::vfs_file_exists(skeletonPath)) {
    return false;
  }
  char folder[512] = {};
  folder_of(meshPath, folder, sizeof(folder));
  std::size_t found = 0U;
  for (std::size_t i = 0U; i < asset_index_count(); ++i) {
    const AssetIndexEntry *entry = asset_index_entry(i);
    char entryFolder[512] = {};
    if ((entry == nullptr) ||
        (entry->kind != content::AssetTypeTag::AnimationController)) {
      continue;
    }
    folder_of(entry->virtualPath, entryFolder, sizeof(entryFolder));
    if ((std::strcmp(entryFolder, folder) != 0) ||
        !controller_drives_skeleton(entry->virtualPath, skeletonPath)) {
      continue;
    }
    if (found == 0U) {
      const int written =
          std::snprintf(out, capacity, "%s", entry->virtualPath);
      if ((written <= 0) || (static_cast<std::size_t>(written) >= capacity)) {
        return false;
      }
    }
    ++found;
  }
  if (found > 1U) {
    char message[640] = {};
    std::snprintf(message, sizeof(message),
                  "%zu animation controllers drive '%s'; the placed mesh "
                  "gets none, so add an Animation component and choose one",
                  found, skeletonPath);
    core::log_message(core::LogLevel::Info, "editor", message);
  }
  return found == 1U;
}

/// Captures a prefab into `record` by instantiating it once and reading
/// every component back. The scratch instance is destroyed at once, so
/// the world is left as it was and the history records the placement as
/// one create.
bool capture_prefab(runtime::World &world, const char *prefabPath,
                    EntityDuplicateRecord *record) noexcept {
  const runtime::Entity scratch =
      runtime::instantiate_prefab(world, prefabPath);
  if (scratch == runtime::kInvalidEntity) {
    return false;
  }
  for (std::size_t i = 0U; i < kComponentEditTypeCount; ++i) {
    record->present[i] = capture_component_snapshot(
        static_cast<ComponentEditType>(i), scratch, &record->components);
  }
  static_cast<void>(world.destroy_entity(scratch));
  return true;
}

/// Fills `record` with a mesh entity: a name from the file, the mesh, and
/// the controller beside it when one drives its skeleton.
bool capture_mesh(const char *meshPath,
                  EntityDuplicateRecord *record) noexcept {
  const std::uint64_t assetId = runtime::editor_request_mesh_asset(meshPath);
  if (assetId == 0ULL) {
    return false;
  }
  record->present[slot_of(ComponentEditType::Transform)] = true;
  record->present[slot_of(ComponentEditType::Name)] = true;
  make_asset_spawn_name(meshPath, &record->components.name);
  record->present[slot_of(ComponentEditType::Mesh)] = true;
  record->components.mesh.meshAssetId = assetId;
  record->components.mesh.meshRef = runtime::editor_asset_ref(assetId);
  char controller[runtime::AnimationComponent::kMaxPathLength + 1U] = {};
  if (find_controller_for_mesh(meshPath, controller, sizeof(controller))) {
    record->present[slot_of(ComponentEditType::Animation)] = true;
    std::snprintf(record->components.animation.controllerPath,
                  sizeof(record->components.animation.controllerPath), "%s",
                  controller);
  }
  return true;
}

} // namespace

bool asset_entry_is_placeable(const AssetIndexEntry &entry) noexcept {
  return (entry.kind == content::AssetTypeTag::Prefab) ||
         (entry.kind == content::AssetTypeTag::Mesh);
}

bool make_asset_place_payload(const AssetIndexEntry &entry,
                              AssetPlacePayload *out) noexcept {
  if ((out == nullptr) || !asset_entry_is_placeable(entry) ||
      (entry.virtualPath[0] == '\0')) {
    return false;
  }
  *out = AssetPlacePayload{};
  std::memcpy(out->virtualPath, entry.virtualPath, sizeof(out->virtualPath));
  out->virtualPath[sizeof(out->virtualPath) - 1U] = '\0';
  out->kind = entry.kind;
  out->isSource = entry.isSource;
  return true;
}

void asset_place_drag_source(const AssetIndexEntry &entry) noexcept {
  AssetPlacePayload payload{};
  if (!make_asset_place_payload(entry, &payload) ||
      !ImGui::BeginDragDropSource()) {
    return;
  }
  ImGui::SetDragDropPayload(kAssetPlacePayloadType, &payload, sizeof(payload));
  ImGui::TextUnformatted(entry.name);
  ImGui::EndDragDropSource();
}

bool accept_asset_place_payload(AssetPlacePayload *out) noexcept {
  const ImGuiPayload *payload =
      ImGui::AcceptDragDropPayload(kAssetPlacePayloadType);
  if ((out == nullptr) || (payload == nullptr) || (payload->Data == nullptr) ||
      (payload->DataSize != static_cast<int>(sizeof(AssetPlacePayload)))) {
    return false;
  }
  std::memcpy(out, payload->Data, sizeof(AssetPlacePayload));
  out->virtualPath[sizeof(out->virtualPath) - 1U] = '\0';
  return true;
}

runtime::Entity
execute_asset_instantiate(const AssetPlacePayload &asset,
                          const EntitySpawnPlacement &placement) noexcept {
  runtime::World *const world = editor_session().world;
  const char *const path = asset.virtualPath;
  if ((world == nullptr) || (path[0] == '\0')) {
    return runtime::kInvalidEntity;
  }
  if (!world_is_editable()) {
    log_refusal(path, "the scene can be edited only while stopped");
    return runtime::kInvalidEntity;
  }
  if ((placement.parent != runtime::kInvalidEntity) &&
      !world->is_alive(placement.parent)) {
    log_refusal(path, "the parent it was dropped on no longer exists");
    return runtime::kInvalidEntity;
  }
  inspector_commit_pending_edit();
  gizmo_commit_gesture();

  auto *command = allocate_command<EntityDuplicateCommand>();
  if (command != nullptr) {
    command->records.reset(new (std::nothrow) EntityDuplicateRecord[1]);
    command->rootRecords.reset(new (std::nothrow) std::size_t[1]);
  }
  if ((command == nullptr) || (command->records == nullptr) ||
      (command->rootRecords == nullptr)) {
    delete command;
    log_refusal(path, "it could not be recorded for undo (out of memory)");
    return runtime::kInvalidEntity;
  }
  EntityDuplicateRecord &record = command->records[0];

  bool captured = false;
  if (asset.kind == content::AssetTypeTag::Prefab) {
    captured = capture_prefab(*world, path, &record);
    if (!captured) {
      log_refusal(path, "the prefab did not load (see the error above)");
    }
  } else if (asset.kind == content::AssetTypeTag::Mesh) {
    // A model places the mesh cooked from it, which the runtime loads.
    char meshPath[512] = {};
    if (!asset.isSource) {
      std::snprintf(meshPath, sizeof(meshPath), "%s", path);
    } else if (!replace_suffix(path, ".mesh", meshPath, sizeof(meshPath)) ||
               !core::vfs_file_exists(meshPath)) {
      log_refusal(path, "the model has no cooked mesh beside it; cook it "
                        "with the asset packer first");
      meshPath[0] = '\0';
    }
    captured = (meshPath[0] != '\0') && capture_mesh(meshPath, &record);
  } else {
    log_refusal(path, "only prefabs, meshes and models can be placed");
  }
  if (!captured) {
    delete command;
    return runtime::kInvalidEntity;
  }

  // A prefab keeps its own rotation and scale; only where it stands
  // changes. Under a parent it sits at the parent's origin, as the Create
  // menu's children do.
  runtime::Transform &transform = record.components.transform;
  record.present[slot_of(ComponentEditType::Transform)] = true;
  transform.parentId = runtime::kInvalidPersistentId;
  if (placement.parent != runtime::kInvalidEntity) {
    transform.position = math::Vec3(0.0F, 0.0F, 0.0F);
    transform.parentId = world->persistent_id(placement.parent);
  } else if (placement.hasPosition) {
    transform.position = placement.position;
  }
  if (!record.present[slot_of(ComponentEditType::Name)]) {
    record.present[slot_of(ComponentEditType::Name)] = true;
    make_asset_spawn_name(path, &record.components.name);
  }
  command->recordCount = 1U;
  command->rootRecords[0] = 0U;
  command->rootCount = 1U;
  if (!execute_duplicate_and_select(command)) {
    log_refusal(path, "the entity could not be created");
    return runtime::kInvalidEntity;
  }
  // A successful execute leaves the command in the history, which owns it
  // from here; its record holds the id the instance was given.
  return world->find_entity_by_persistent_id(command->records[0].persistentId);
}

} // namespace engine::editor
