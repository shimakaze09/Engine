// Implements the editor's undoable entity-lifetime commands: create
// (empty, from an asset, from a built-in primitive), delete and duplicate,
// each capturing whole subtrees so undo restores identity and hierarchy.
// Split from editor_commands.cpp, which keeps component, reparent and
// gizmo edits.

#include "editor_commands.h"

#include "editor_material_edit.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#include "engine/content/asset_metadata.h"
#include "engine/core/logging.h"
#include "engine/editor/editor_camera.h"
#include "engine/math/transform.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/mesh_primitives.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/primitive_collider.h"
#include "engine/runtime/world.h"

#include "engine/editor/command_history.h"

namespace engine::editor {

bool EntityCreateCommand::execute() noexcept {
  runtime::World *const world = editor_session().world;
  if (world == nullptr) {
    return false;
  }
  const runtime::Entity entity =
      (persistentId == runtime::kInvalidPersistentId)
          ? world->create_scene_object(transform)
          : world->create_scene_object_with_persistent_id(persistentId,
                                                          transform);
  if (entity == runtime::kInvalidEntity) {
    core::log_message(core::LogLevel::Error, "editor",
                      "entity create command could not create the entity");
    return false;
  }
  persistentId = world->persistent_id(entity);
  if (name.name[0] == '\0') {
    make_default_entity_name(entity.index, &name);
  }
  // Any failed insertion rolls the whole creation back so the history
  // never records a partially constructed entity.
  bool ok = world->add_name_component(entity, name);
  if (ok && hasMesh) {
    ok = world->add_mesh_component(entity, mesh);
  }
  if (ok && hasCollider) {
    ok = world->add_collider(entity, colliderComponent);
  }
  if (!ok) {
    core::log_message(core::LogLevel::Error, "editor",
                      "entity create command rolled back a partial entity");
    static_cast<void>(world->destroy_entity(entity));
    return false;
  }
  return true;
}

bool EntityCreateCommand::undo() noexcept {
  runtime::World *const world = editor_session().world;
  if ((world == nullptr) || (persistentId == runtime::kInvalidPersistentId)) {
    return false;
  }
  const runtime::Entity entity =
      world->find_entity_by_persistent_id(persistentId);
  if (entity == runtime::kInvalidEntity) {
    return false;
  }
  return world->destroy_entity(entity);
}

/// Collects the entity's transform subtree into members (root first,
/// every parent before its children) with an iterative breadth-first
/// frontier and per-index visited marks, so corrupted parent links
/// (cycles, self-parenting) are captured once and 1000+-deep chains
/// cannot grow the call stack; returns the member count (bounded by
/// capacity). Builds a single parentId -> children index up front (one
/// world.for_each_alive pass) instead of rescanning every alive entity
/// per BFS member, so a subtree of S members in a world of N entities
/// costs O(N log N + S log N) rather than O(S * N); the per-parent child
/// order is unchanged (ascending entity
/// index), so member order is identical to the prior full-scan walk.
static std::size_t collect_subtree_members(runtime::World &world,
                                           runtime::Entity root,
                                           runtime::Entity *members,
                                           std::size_t capacity,
                                           bool *visited) noexcept {
  if ((members == nullptr) || (visited == nullptr) || (capacity == 0U) ||
      !world.is_alive(root)) {
    return 0U;
  }

  // Cold editor path (not per-frame): a heap-allocated index is fine here,
  // unlike the hot-path ECS/physics/render-prep code CLAUDE.md restricts.
  std::vector<std::pair<runtime::PersistentId, runtime::Entity>> byParent;
  world.for_each_alive([&](runtime::Entity candidate) {
    runtime::Transform transform{};
    if (world.get_transform(candidate, &transform) &&
        (transform.parentId != runtime::kInvalidPersistentId)) {
      byParent.emplace_back(transform.parentId, candidate);
    }
  });
  std::sort(byParent.begin(), byParent.end(),
            [](const auto &lhs, const auto &rhs) noexcept {
              if (lhs.first != rhs.first) {
                return lhs.first < rhs.first;
              }
              return lhs.second.index < rhs.second.index;
            });

  std::size_t count = 0U;
  members[count++] = root;
  visited[root.index] = true;
  for (std::size_t cursor = 0U; cursor < count; ++cursor) {
    const runtime::PersistentId ownId = world.persistent_id(members[cursor]);
    if (ownId == runtime::kInvalidPersistentId) {
      continue;
    }
    const auto range = std::equal_range(
        byParent.begin(), byParent.end(),
        std::pair<runtime::PersistentId, runtime::Entity>{ownId, {}},
        [](const auto &lhs, const auto &rhs) noexcept {
          return lhs.first < rhs.first;
        });
    for (auto it = range.first; (it != range.second) && (count < capacity);
         ++it) {
      const runtime::Entity candidate = it->second;
      if (!visited[candidate.index]) {
        visited[candidate.index] = true;
        members[count++] = candidate;
      }
    }
  }
  return count;
}

/// Re-creates records [begin, end) under their persistent ids, parents
/// before children. All or nothing: on a failure every member this call
/// created is destroyed again and it returns false.
static bool restore_delete_records(runtime::World &world,
                                   const EntityDeleteRecord *records,
                                   std::size_t begin,
                                   std::size_t end) noexcept {
  constexpr std::size_t transformSlot =
      static_cast<std::size_t>(ComponentEditType::Transform);
  std::size_t restored = begin;
  bool ok = true;
  for (std::size_t i = begin; ok && (i < end); ++i) {
    const EntityDeleteRecord &record = records[i];
    const runtime::Entity entity =
        record.present[transformSlot]
            ? world.create_scene_object_with_persistent_id(
                  record.persistentId, record.components.transform)
            : world.create_entity_with_persistent_id(record.persistentId);
    if (entity == runtime::kInvalidEntity) {
      ok = false;
      break;
    }
    ++restored;
    for (std::size_t typeIndex = 0U; typeIndex < kComponentEditTypeCount;
         ++typeIndex) {
      if (!record.present[typeIndex] || (typeIndex == transformSlot)) {
        continue;
      }
      if (!apply_component_snapshot(static_cast<ComponentEditType>(typeIndex),
                                    entity, true, record.components)) {
        ok = false;
        break;
      }
    }
  }
  if (ok) {
    return true;
  }
  // Children before parents: destroy_entity takes whole transform
  // subtrees, so reverse order never double-frees a member.
  for (std::size_t i = restored; i > begin; --i) {
    const runtime::Entity member =
        world.find_entity_by_persistent_id(records[i - 1U].persistentId);
    if (member != runtime::kInvalidEntity) {
      static_cast<void>(world.destroy_entity(member));
    }
  }
  return false;
}

bool EntityDeleteCommand::execute() noexcept {
  runtime::World *const world = editor_session().world;
  if ((world == nullptr) || (recordCount == 0U) || (rootCount == 0U)) {
    return false;
  }
  // Every root must resolve before any is destroyed, so a stale root
  // refuses the whole command instead of deleting part of the forest.
  for (std::size_t k = 0U; k < rootCount; ++k) {
    if (world->find_entity_by_persistent_id(
            records[rootRecords[k]].persistentId) == runtime::kInvalidEntity) {
      return false;
    }
  }
  for (std::size_t k = 0U; k < rootCount; ++k) {
    const runtime::Entity root = world->find_entity_by_persistent_id(
        records[rootRecords[k]].persistentId);
    if (!world->destroy_entity(root)) {
      // Put back the roots already destroyed, so a failed step leaves the
      // world as it found it.
      if (!restore_delete_records(*world, records.get(), 0U, rootRecords[k])) {
        core::log_message(core::LogLevel::Error, "editor",
                          "entity delete failed part-way and the deleted "
                          "entities could not all be restored");
      }
      return false;
    }
  }
  return true;
}

bool EntityDeleteCommand::undo() noexcept {
  runtime::World *const world = editor_session().world;
  if (world == nullptr) {
    return false;
  }
  if (!restore_delete_records(*world, records.get(), 0U, recordCount)) {
    core::log_message(
        core::LogLevel::Error, "editor",
        "entity delete undo could not restore the subtree — rolled back");
    return false;
  }
  return true;
}

runtime::Entity execute_entity_create() noexcept {
  runtime::World *const world = editor_session().world;
  if (world == nullptr) {
    return runtime::kInvalidEntity;
  }
  auto *command = allocate_command<EntityCreateCommand>();
  if (command == nullptr) {
    core::log_message(core::LogLevel::Error, "editor",
                      "entity create refused: it could not be recorded for "
                      "undo (out of memory)");
    return runtime::kInvalidEntity;
  }
  if (!editor_session().commandHistory.execute(command)) {
    return runtime::kInvalidEntity;
  }
  return world->find_entity_by_persistent_id(command->persistentId);
}

/// Copies the file stem of a virtual asset path into a name component
/// ("assets/props/rock.mesh" names the spawn "rock").
static void make_asset_spawn_name(const char *virtualPath,
                                  runtime::NameComponent *outName) noexcept {
  if ((virtualPath == nullptr) || (outName == nullptr)) {
    return;
  }
  const char *stem = virtualPath;
  for (const char *cursor = virtualPath; *cursor != '\0'; ++cursor) {
    if ((*cursor == '/') || (*cursor == '\\')) {
      stem = cursor + 1;
    }
  }
  const int written =
      std::snprintf(outName->name, sizeof(outName->name), "%s", stem);
  // A long asset filename still spawns (naming is cosmetic, not fatal) but
  // must not clip into NameComponent::kMaxNameLength silently: surface
  // it once so the author can see why the entity name was
  // shortened instead of it just quietly not matching the file.
  if ((written < 0) ||
      (static_cast<std::size_t>(written) >= sizeof(outName->name))) {
    char message[256] = {};
    std::snprintf(message, sizeof(message),
                  "asset spawn name truncated to %zu chars (source: %s)",
                  sizeof(outName->name) - 1U, virtualPath);
    core::log_message(core::LogLevel::Warning, "editor", message);
  }
  char *dot = std::strrchr(outName->name, '.');
  if ((dot != nullptr) && (dot != outName->name)) {
    *dot = '\0';
  }
}

runtime::Entity
execute_asset_spawn(const char *virtualPath,
                    const runtime::Transform &transform) noexcept {
  runtime::World *const world = editor_session().world;
  if ((world == nullptr) || (virtualPath == nullptr) ||
      (virtualPath[0] == '\0')) {
    return runtime::kInvalidEntity;
  }

  const std::uint64_t assetId = runtime::editor_request_mesh_asset(virtualPath);
  if (assetId == 0ULL) {
    return runtime::kInvalidEntity;
  }

  auto *command = allocate_command<EntityCreateCommand>();
  if (command == nullptr) {
    core::log_message(core::LogLevel::Error, "editor",
                      "asset spawn refused: it could not be recorded for "
                      "undo (out of memory)");
    return runtime::kInvalidEntity;
  }

  command->transform = transform;
  make_asset_spawn_name(virtualPath, &command->name);
  command->hasMesh = true;
  command->mesh.meshAssetId = assetId;
  command->mesh.meshRef = runtime::editor_asset_ref(assetId);
  if (!editor_session().commandHistory.execute(command)) {
    return runtime::kInvalidEntity;
  }
  return world->find_entity_by_persistent_id(command->persistentId);
}

bool execute_asset_open(const AssetIndexEntry &entry) noexcept {
  std::snprintf(editor_session().selectedAssetPath,
                sizeof(editor_session().selectedAssetPath), "%s", entry.osPath);

  switch (resolve_asset_open_action(entry.kind, entry.isSource)) {
  case AssetOpenAction::SpawnMesh: {
    const renderer::CameraState cam =
        editor_camera_state(editor_session().editorCamera);
    runtime::Transform transform{};
    transform.position = math::Vec3(cam.target.x, 0.5F, cam.target.z);
    const runtime::Entity spawned =
        execute_asset_spawn(entry.virtualPath, transform);
    if (spawned != runtime::kInvalidEntity) {
      select_entity(spawned, false);
    }
    return spawned != runtime::kInvalidEntity;
  }
  case AssetOpenAction::OpenScene:
    // Gated: proceeds immediately when the current document is clean, or
    // arms the unsaved-change prompt and defers.
    request_scene_open(entry.osPath);
    return true;
  case AssetOpenAction::EditMaterial:
    open_material_editor(entry.virtualPath);
    return true;
  case AssetOpenAction::SelectOnly:
  default:
    return true;
  }
}

/// Per-primitive spawn description: display name, builtin mesh path,
/// resting height, fallback collider, and hull provenance.
struct PrimitiveSpawnDesc final {
  const char *name = nullptr;
  const char *builtinPath = nullptr;
  float groundY = 0.5F;
  math::ColliderShape fallbackShape = math::ColliderShape::AABB;
  math::Vec3 halfExtents = math::Vec3(0.5F, 0.5F, 0.5F);
  math::Vec3 colliderLocalPosition = math::Vec3(0.0F, 0.0F, 0.0F);
  math::HullSource hullSource = math::HullSource::None;
};

/// Returns the spawn description for a built-in blockout primitive.
static PrimitiveSpawnDesc
primitive_spawn_desc(EditorPrimitive primitive) noexcept {
  PrimitiveSpawnDesc desc{};
  switch (primitive) {
  case EditorPrimitive::Cube:
    desc.name = "Cube";
    desc.builtinPath = "builtin://cube";
    break;
  case EditorPrimitive::Sphere:
    desc.name = "Sphere";
    desc.builtinPath = "builtin://sphere";
    desc.fallbackShape = math::ColliderShape::Sphere;
    break;
  case EditorPrimitive::Cylinder:
    desc.name = "Cylinder";
    desc.builtinPath = "builtin://cylinder";
    desc.fallbackShape = math::ColliderShape::Capsule;
    desc.hullSource = math::HullSource::Cylinder;
    break;
  case EditorPrimitive::Capsule:
    desc.name = "Capsule";
    desc.builtinPath = "builtin://capsule";
    desc.groundY = 1.0F;
    desc.fallbackShape = math::ColliderShape::Capsule;
    break;
  case EditorPrimitive::Pyramid:
    desc.name = "Pyramid";
    desc.builtinPath = "builtin://pyramid";
    desc.halfExtents = math::Vec3(0.5F, 0.5F, 0.58F);
    desc.hullSource = math::HullSource::Pyramid;
    break;
  case EditorPrimitive::Plane:
    desc.name = "Plane";
    desc.builtinPath = "builtin://plane";
    // Both heights are derived from where the mesh puts its surface, so
    // the spawn lands its ground on zero and the collider's top meets
    // that ground, whatever the primitive does. They were hand-written
    // offsets that cancelled a surface half a metre above the origin,
    // which is the sort of correction that goes stale silently. Subtracted
    // from zero rather than negated: negating a zero surface height gives
    // negative zero, which compares equal but hashes and saves as a
    // different value than the zero an author would type.
    desc.groundY = 0.0F - renderer::kBuiltinPlaneSurfaceY;
    desc.halfExtents = math::Vec3(5.0F, 0.1F, 5.0F);
    desc.colliderLocalPosition =
        math::Vec3(0.0F, renderer::kBuiltinPlaneSurfaceY - 0.1F, 0.0F);
    break;
  }
  return desc;
}

runtime::Entity execute_primitive_spawn(EditorPrimitive primitive) noexcept {
  runtime::World *const world = editor_session().world;
  if (world == nullptr) {
    return runtime::kInvalidEntity;
  }

  const PrimitiveSpawnDesc desc = primitive_spawn_desc(primitive);
  if ((desc.name == nullptr) || (desc.builtinPath == nullptr)) {
    return runtime::kInvalidEntity;
  }

  auto *command = allocate_command<EntityCreateCommand>();
  if (command == nullptr) {
    return runtime::kInvalidEntity;
  }

  const renderer::CameraState cam =
      editor_camera_state(editor_session().editorCamera);
  command->transform.position =
      math::Vec3(cam.target.x, desc.groundY, cam.target.z);
  std::snprintf(command->name.name, sizeof(command->name.name), "%s",
                desc.name);
  command->hasMesh = true;
  command->mesh.meshAssetId =
      content::make_asset_id_from_path(desc.builtinPath);
  // A primitive's identity is derived from its path rather than looked
  // up: it ships with the engine, so it is the same asset in every build
  // and needs no catalog record to be nameable in a saved scene.
  command->mesh.meshRef =
      content::asset_ref_primary(content::builtin_asset_guid(desc.builtinPath));
  command->hasCollider = true;
  command->colliderComponent.shape = desc.fallbackShape;
  command->colliderComponent.halfExtents = desc.halfExtents;
  command->colliderComponent.localPosition = desc.colliderLocalPosition;
  // The authored fallback above stands when the primitive names no hull;
  // otherwise the runtime sizes and tags the collider from the one hull
  // provenance every install path rebuilds from.
  static_cast<void>(runtime::apply_primitive_hull(desc.hullSource,
                                                  &command->colliderComponent));
  if (!editor_session().commandHistory.execute(command)) {
    return runtime::kInvalidEntity;
  }
  return world->find_entity_by_persistent_id(command->persistentId);
}

/// True when an ancestor of `entity` is marked in `listed`. The walk is
/// bounded by the alive count, so a corrupted parent cycle ends it.
static bool has_listed_ancestor(const runtime::World &world,
                                runtime::Entity entity,
                                const bool *listed) noexcept {
  runtime::Entity cursor = entity;
  const std::size_t bound = world.alive_entity_count();
  for (std::size_t step = 0U; step < bound; ++step) {
    runtime::Transform transform{};
    if (!world.get_transform(cursor, &transform) ||
        (transform.parentId == runtime::kInvalidPersistentId)) {
      return false;
    }
    cursor = world.find_entity_by_persistent_id(transform.parentId);
    if ((cursor == runtime::kInvalidEntity) || (cursor == entity)) {
      return false;
    }
    if (listed[cursor.index]) {
      return true;
    }
  }
  return false;
}

EntityDeleteCommand *
build_entity_delete_command(const runtime::Entity *entities,
                            std::size_t count) noexcept {
  runtime::World *const world = editor_session().world;
  if ((world == nullptr) || (entities == nullptr) || (count == 0U)) {
    return nullptr;
  }
  constexpr std::size_t kSlots = runtime::World::kMaxEntities + 1U;
  const std::size_t capacity = world->alive_entity_count();
  std::unique_ptr<bool[]> listed(new (std::nothrow) bool[kSlots]());
  std::unique_ptr<bool[]> visited(new (std::nothrow) bool[kSlots]());
  std::unique_ptr<runtime::Entity[]> members(
      new (std::nothrow) runtime::Entity[(capacity > 0U) ? capacity : 1U]);
  std::unique_ptr<std::size_t[]> rootRecords(new (std::nothrow)
                                                 std::size_t[count]);
  if ((listed == nullptr) || (visited == nullptr) || (members == nullptr) ||
      (rootRecords == nullptr)) {
    return nullptr;
  }
  for (std::size_t i = 0U; i < count; ++i) {
    if (world->is_alive(entities[i])) {
      listed[entities[i].index] = true;
    }
  }

  // Each listed entity without a listed ancestor is a root and takes its
  // subtree; a repeat, or a member of an earlier root's subtree, is
  // already visited.
  std::size_t memberCount = 0U;
  std::size_t rootCount = 0U;
  for (std::size_t i = 0U; i < count; ++i) {
    const runtime::Entity entity = entities[i];
    if (!world->is_alive(entity) || visited[entity.index] ||
        has_listed_ancestor(*world, entity, listed.get())) {
      continue;
    }
    const std::size_t taken =
        collect_subtree_members(*world, entity, &members[memberCount],
                                capacity - memberCount, visited.get());
    if (taken == 0U) {
      continue;
    }
    rootRecords[rootCount++] = memberCount;
    memberCount += taken;
  }
  if (rootCount == 0U) {
    return nullptr;
  }

  auto *command = allocate_command<EntityDeleteCommand>();
  if (command == nullptr) {
    return nullptr;
  }
  command->records.reset(new (std::nothrow) EntityDeleteRecord[memberCount]);
  if (command->records == nullptr) {
    delete command;
    return nullptr;
  }
  for (std::size_t i = 0U; i < memberCount; ++i) {
    EntityDeleteRecord &record = command->records[i];
    record.persistentId = world->persistent_id(members[i]);
    for (std::size_t typeIndex = 0U; typeIndex < kComponentEditTypeCount;
         ++typeIndex) {
      record.present[typeIndex] =
          capture_component_snapshot(static_cast<ComponentEditType>(typeIndex),
                                     members[i], &record.components);
    }
  }
  command->recordCount = memberCount;
  command->rootRecords = std::move(rootRecords);
  command->rootCount = rootCount;
  return command;
}

EntityDeleteCommand *
build_entity_delete_command(runtime::Entity entity) noexcept {
  return build_entity_delete_command(&entity, 1U);
}

/// Writes a name no live entity holds into `name`: the source name, or
/// it with " (2)", " (3)" and so on appended, cut to fit the component.
void make_unique_entity_name(const runtime::World &world,
                             runtime::NameComponent *name) noexcept {
  if (name->name[0] == '\0') {
    return;
  }
  if (world.find_entity_by_name(name->name) == runtime::kInvalidEntity) {
    return;
  }
  // A base that already ends in " (n)" is extended rather than parsed, so
  // "Coin (2)" duplicates to "Coin (2) (2)": the authored text is kept
  // whole, which matters more here than a tidy sequence.
  char base[sizeof(name->name)] = {};
  std::snprintf(base, sizeof(base), "%s", name->name);
  for (unsigned suffix = 2U; suffix < 1000U; ++suffix) {
    char candidate[sizeof(name->name)] = {};
    // Trim the base so the suffix always fits: a name that cannot take
    // one whole would otherwise be a truncated copy of another entity's.
    char suffixText[12] = {};
    std::snprintf(suffixText, sizeof(suffixText), " (%u)", suffix);
    const std::size_t suffixLength = std::strlen(suffixText);
    const std::size_t room = sizeof(candidate) - 1U - suffixLength;
    std::size_t baseLength = std::strlen(base);
    if (baseLength > room) {
      baseLength = room;
    }
    std::memcpy(candidate, base, baseLength);
    std::memcpy(candidate + baseLength, suffixText, suffixLength + 1U);
    if (world.find_entity_by_name(candidate) == runtime::kInvalidEntity) {
      std::snprintf(name->name, sizeof(name->name), "%s", candidate);
      return;
    }
  }
}

bool EntityDuplicateCommand::execute() noexcept {
  runtime::World *const world = editor_session().world;
  if ((world == nullptr) || (recordCount == 0U)) {
    return false;
  }
  constexpr std::size_t transformSlot =
      static_cast<std::size_t>(ComponentEditType::Transform);
  constexpr std::size_t nameSlot =
      static_cast<std::size_t>(ComponentEditType::Name);

  std::size_t created = 0U;
  bool ok = true;
  for (std::size_t i = 0U; ok && (i < recordCount); ++i) {
    EntityDuplicateRecord &record = records[i];
    ComponentEditSnapshot components = record.components;
    // Internal parent links point at the copies, not the originals; the
    // root's link is left as captured, so it stays under the same parent.
    if (record.parentRecord != EntityDuplicateRecord::kNoParentRecord) {
      components.transform.parentId = records[record.parentRecord].persistentId;
    }
    if ((i == 0U) && record.present[nameSlot]) {
      make_unique_entity_name(*world, &components.name);
      record.components.name = components.name;
    }
    const runtime::Entity entity =
        (record.persistentId == runtime::kInvalidPersistentId)
            ? (record.present[transformSlot]
                   ? world->create_scene_object(components.transform)
                   : world->create_entity())
            : (record.present[transformSlot]
                   ? world->create_scene_object_with_persistent_id(
                         record.persistentId, components.transform)
                   : world->create_entity_with_persistent_id(
                         record.persistentId));
    if (entity == runtime::kInvalidEntity) {
      ok = false;
      break;
    }
    record.persistentId = world->persistent_id(entity);
    ++created;
    for (std::size_t typeIndex = 0U; typeIndex < kComponentEditTypeCount;
         ++typeIndex) {
      if (!record.present[typeIndex] || (typeIndex == transformSlot)) {
        continue;
      }
      if (!apply_component_snapshot(static_cast<ComponentEditType>(typeIndex),
                                    entity, true, components)) {
        ok = false;
        break;
      }
    }
  }
  if (!ok) {
    for (std::size_t i = created; i > 0U; --i) {
      const runtime::Entity member =
          world->find_entity_by_persistent_id(records[i - 1U].persistentId);
      if (member != runtime::kInvalidEntity) {
        static_cast<void>(world->destroy_entity(member));
      }
    }
    core::log_message(core::LogLevel::Error, "editor",
                      "entity duplicate could not create the copy — rolled "
                      "back");
    return false;
  }
  return true;
}

bool EntityDuplicateCommand::undo() noexcept {
  runtime::World *const world = editor_session().world;
  if ((world == nullptr) || (recordCount == 0U)) {
    return false;
  }
  // The root's destroy takes its whole transform subtree, which is every
  // member this command created.
  const runtime::Entity root =
      world->find_entity_by_persistent_id(records[0].persistentId);
  if (root == runtime::kInvalidEntity) {
    return false;
  }
  return world->destroy_entity(root);
}

EntityDuplicateCommand *
build_entity_duplicate_command(runtime::Entity entity) noexcept {
  runtime::World *const world = editor_session().world;
  if ((world == nullptr) || !world->is_alive(entity)) {
    return nullptr;
  }
  const std::size_t capacity = world->alive_entity_count();
  std::unique_ptr<runtime::Entity[]> members(new (std::nothrow)
                                                 runtime::Entity[capacity]);
  std::unique_ptr<bool[]> visited(
      new (std::nothrow) bool[runtime::World::kMaxEntities + 1U]());
  if ((members == nullptr) || (visited == nullptr)) {
    return nullptr;
  }
  const std::size_t count = collect_subtree_members(
      *world, entity, members.get(), capacity, visited.get());
  if (count == 0U) {
    return nullptr;
  }
  auto *command = allocate_command<EntityDuplicateCommand>();
  if (command == nullptr) {
    return nullptr;
  }
  command->records.reset(new (std::nothrow) EntityDuplicateRecord[count]);
  if (command->records == nullptr) {
    delete command;
    return nullptr;
  }
  for (std::size_t i = 0U; i < count; ++i) {
    EntityDuplicateRecord &record = command->records[i];
    for (std::size_t typeIndex = 0U; typeIndex < kComponentEditTypeCount;
         ++typeIndex) {
      record.present[typeIndex] =
          capture_component_snapshot(static_cast<ComponentEditType>(typeIndex),
                                     members[i], &record.components);
    }
    if (i == 0U) {
      continue;
    }
    // collect_subtree_members walks parents before children, so the
    // parent of every member past the root is already recorded.
    const runtime::PersistentId parentId = record.components.transform.parentId;
    for (std::size_t candidate = 0U; candidate < i; ++candidate) {
      if (world->persistent_id(members[candidate]) == parentId) {
        record.parentRecord = candidate;
        break;
      }
    }
  }
  command->recordCount = count;
  return command;
}

runtime::Entity execute_entity_duplicate(runtime::Entity entity) noexcept {
  runtime::World *const world = editor_session().world;
  if (world == nullptr) {
    return runtime::kInvalidEntity;
  }
  inspector_commit_pending_edit();
  EntityDuplicateCommand *const command =
      build_entity_duplicate_command(entity);
  if (command == nullptr) {
    core::log_message(core::LogLevel::Error, "editor",
                      "entity duplicate refused: it could not be recorded "
                      "for undo (out of memory or not alive)");
    return runtime::kInvalidEntity;
  }
  if (!editor_session().commandHistory.execute(command)) {
    return runtime::kInvalidEntity;
  }
  return world->find_entity_by_persistent_id(command->records[0].persistentId);
}

bool execute_entity_delete(runtime::Entity entity) noexcept {
  inspector_commit_pending_edit();
  EntityDeleteCommand *const command = build_entity_delete_command(entity);
  if (command == nullptr) {
    core::log_message(core::LogLevel::Error, "editor",
                      "entity delete refused: it could not be recorded for "
                      "undo (out of memory or not alive)");
    return false;
  }
  return editor_session().commandHistory.execute(command);
}

bool execute_selection_delete() noexcept {
  // A gesture open on a member would otherwise record against an entity
  // this command is about to remove.
  inspector_commit_pending_edit();
  gizmo_commit_gesture();
  prune_entity_selection();
  EditorSession &session = editor_session();
  const runtime::Entity primary = selected_entity();
  const runtime::Entity *entities = session.selectedEntities.data();
  std::size_t count = session.selectedEntityCount;
  if (count == 0U) {
    if (primary == runtime::kInvalidEntity) {
      return false;
    }
    entities = &primary;
    count = 1U;
  }
  EntityDeleteCommand *const command =
      build_entity_delete_command(entities, count);
  if (command == nullptr) {
    core::log_message(core::LogLevel::Error, "editor",
                      "delete refused: the selection could not be recorded "
                      "for undo (out of memory or nothing alive)");
    return false;
  }
  if (!session.commandHistory.execute(command)) {
    return false;
  }
  clear_entity_selection();
  return true;
}

} // namespace engine::editor
