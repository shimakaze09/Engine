// Pins the editor's Duplicate through its production command: every
// persistent component of the source is copied, the copy is a new entity
// with its own persistent id and a name no other entity holds, a
// duplicated parent brings its children with their parent links pointing
// at the copies (not the originals), the source keeps its own parent, and
// the whole duplication is one undo step that redo restores under the
// same persistent ids.

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "../test_harness.h"
#include "editor_commands.h"
#include "editor_session.h"
#include "engine/editor/command_history.h"
#include "engine/runtime/world.h"

namespace {

using engine::editor::ComponentEditType;
using engine::runtime::Entity;
using engine::runtime::NameComponent;
using engine::runtime::PersistentId;
using engine::runtime::Transform;
using engine::runtime::World;

engine::tests::TestContext g_tests;

void check(bool condition, const char *name) noexcept {
  g_tests.check(condition, name);
}

/// Binds a fresh world to the editor session and restores it after.
struct SessionWorldScope final {
  World *previousWorld = nullptr;

  explicit SessionWorldScope(World *world) noexcept {
    previousWorld = engine::editor::editor_session().world;
    engine::editor::editor_session().world = world;
  }

  ~SessionWorldScope() noexcept {
    engine::editor::editor_session().commandHistory.clear();
    engine::editor::editor_session().world = previousWorld;
  }
};

Entity add_named(World &world, const char *name,
                 const Transform &transform) noexcept {
  const Entity entity = world.create_scene_object(transform);
  if (entity == engine::runtime::kInvalidEntity) {
    return entity;
  }
  NameComponent nameComponent{};
  std::snprintf(nameComponent.name, sizeof(nameComponent.name), "%s", name);
  if (!world.add_name_component(entity, nameComponent)) {
    return engine::runtime::kInvalidEntity;
  }
  return entity;
}

const char *name_of(const World &world, Entity entity) noexcept {
  static NameComponent name{};
  name = NameComponent{};
  static_cast<void>(world.get_name_component(entity, &name));
  return name.name;
}

/// A single entity with a mesh, a rigid body and a collider: every
/// component is copied, the copy is its own entity, and the name is made
/// unique. One undo step removes it; redo brings it back unchanged.
void test_single_entity() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    g_tests.fail("allocate the world");
    return;
  }
  SessionWorldScope scope(world.get());

  Transform transform{};
  transform.position = engine::math::Vec3(1.0F, 2.0F, 3.0F);
  const Entity source = add_named(*world, "Coin", transform);
  engine::runtime::MeshComponent mesh{};
  mesh.meshAssetId = 0x1234ULL;
  mesh.roughness = 0.25F;
  engine::runtime::RigidBody body{};
  body.inverseMass = 2.0F;
  body.gravityScale = 0.0F;
  body.inertiaAuthored = true;
  engine::runtime::Collider collider{};
  collider.halfExtents = engine::math::Vec3(0.3F, 0.6F, 0.3F);
  check((source != engine::runtime::kInvalidEntity) &&
            world->add_mesh_component(source, mesh) &&
            world->add_rigid_body(source, body) &&
            world->add_collider(source, collider),
        "the source entity is built");

  const std::size_t aliveBefore = world->alive_entity_count();
  const Entity copy = engine::editor::execute_entity_duplicate(source);
  check((copy != engine::runtime::kInvalidEntity) && (copy != source),
        "duplicate returns a new entity");
  check(world->alive_entity_count() == (aliveBefore + 1U),
        "exactly one entity is added");
  check(world->persistent_id(copy) != world->persistent_id(source),
        "the copy has its own persistent id");
  check(std::strcmp(name_of(*world, copy), "Coin (2)") == 0,
        "the copy's name is unique");
  check(std::strcmp(name_of(*world, source), "Coin") == 0,
        "the source keeps its name");

  engine::runtime::MeshComponent copiedMesh{};
  engine::runtime::RigidBody copiedBody{};
  engine::runtime::Collider copiedCollider{};
  Transform copiedTransform{};
  check(world->get_mesh_component(copy, &copiedMesh) &&
            (copiedMesh.meshAssetId == mesh.meshAssetId) &&
            (copiedMesh.roughness == mesh.roughness),
        "the mesh component is copied");
  check(world->get_rigid_body(copy, &copiedBody) &&
            (copiedBody.inverseMass == body.inverseMass) &&
            (copiedBody.gravityScale == body.gravityScale),
        "the rigid body is copied, gravity scale included");
  check(world->get_collider(copy, &copiedCollider) &&
            (copiedCollider.halfExtents.x == collider.halfExtents.x),
        "the collider is copied");
  check(world->get_transform(copy, &copiedTransform) &&
            (copiedTransform.position.x == 1.0F) &&
            (copiedTransform.position.z == 3.0F),
        "the transform is copied");

  auto &history = engine::editor::editor_session().commandHistory;
  const PersistentId copyId = world->persistent_id(copy);
  check(history.can_undo() && history.undo(), "the duplicate undoes");
  check(world->alive_entity_count() == aliveBefore,
        "undo removes the copy");
  check(world->find_entity_by_persistent_id(copyId) ==
            engine::runtime::kInvalidEntity,
        "the copy is gone after undo");
  check(history.redo(), "the duplicate redoes");
  const Entity restored = world->find_entity_by_persistent_id(copyId);
  check(restored != engine::runtime::kInvalidEntity,
        "redo restores the copy under its persistent id");
  check(world->get_mesh_component(restored, &copiedMesh) &&
            (copiedMesh.meshAssetId == mesh.meshAssetId),
        "the restored copy keeps its components");
  check(std::strcmp(name_of(*world, restored), "Coin (2)") == 0,
        "the restored copy keeps its unique name");
}

/// A parent with two children under a grandparent: the copy carries the
/// subtree, the copied children point at the copied parent, the copied
/// root keeps the source's parent, and one undo removes all three.
void test_subtree() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    g_tests.fail("allocate the world");
    return;
  }
  SessionWorldScope scope(world.get());

  const Entity grandparent = add_named(*world, "Root", Transform{});
  Transform childTransform{};
  childTransform.parentId = world->persistent_id(grandparent);
  const Entity parent = add_named(*world, "Group", childTransform);
  Transform leafTransform{};
  leafTransform.parentId = world->persistent_id(parent);
  const Entity childA = add_named(*world, "Left", leafTransform);
  const Entity childB = add_named(*world, "Right", leafTransform);
  check((grandparent != engine::runtime::kInvalidEntity) &&
            (parent != engine::runtime::kInvalidEntity) &&
            (childA != engine::runtime::kInvalidEntity) &&
            (childB != engine::runtime::kInvalidEntity),
        "the source subtree is built");

  const std::size_t aliveBefore = world->alive_entity_count();
  const Entity copy = engine::editor::execute_entity_duplicate(parent);
  check(copy != engine::runtime::kInvalidEntity, "the subtree duplicates");
  check(world->alive_entity_count() == (aliveBefore + 3U),
        "the parent and both children are copied");

  Transform copiedRoot{};
  check(world->get_transform(copy, &copiedRoot) &&
            (copiedRoot.parentId == world->persistent_id(grandparent)),
        "the copied root keeps the source's parent");

  const PersistentId copyId = world->persistent_id(copy);
  std::size_t copiedChildren = 0U;
  world->for_each_alive([&](Entity candidate) noexcept {
    Transform transform{};
    if (world->get_transform(candidate, &transform) &&
        (transform.parentId == copyId)) {
      ++copiedChildren;
    }
  });
  check(copiedChildren == 2U, "both copied children point at the copy");

  std::size_t sourceChildren = 0U;
  const PersistentId sourceId = world->persistent_id(parent);
  world->for_each_alive([&](Entity candidate) noexcept {
    Transform transform{};
    if (world->get_transform(candidate, &transform) &&
        (transform.parentId == sourceId)) {
      ++sourceChildren;
    }
  });
  check(sourceChildren == 2U, "the source keeps exactly its own children");

  auto &history = engine::editor::editor_session().commandHistory;
  check(history.undo(), "the subtree duplicate undoes");
  check(world->alive_entity_count() == aliveBefore,
        "one undo removes the whole copied subtree");
}

/// Duplicating twice makes two differently named copies, and a name that
/// cannot take a suffix whole is still never a truncated duplicate.
void test_names() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    g_tests.fail("allocate the world");
    return;
  }
  SessionWorldScope scope(world.get());

  const Entity source = add_named(*world, "Coin", Transform{});
  const Entity first = engine::editor::execute_entity_duplicate(source);
  const Entity second = engine::editor::execute_entity_duplicate(source);
  check((first != engine::runtime::kInvalidEntity) &&
            (second != engine::runtime::kInvalidEntity) &&
            (std::strcmp(name_of(*world, first), "Coin (2)") == 0),
        "the first copy is Coin (2)");
  check(std::strcmp(name_of(*world, second), "Coin (3)") == 0,
        "the second copy is Coin (3)");

  // A name filling the component leaves no room for a suffix: the copy's
  // name is the base cut to fit it, which is a name of its own.
  char longName[NameComponent::kMaxNameLength + 1U] = {};
  for (std::size_t i = 0U; i < NameComponent::kMaxNameLength; ++i) {
    longName[i] = 'x';
  }
  const Entity longSource = add_named(*world, longName, Transform{});
  const Entity longCopy = engine::editor::execute_entity_duplicate(longSource);
  check((longCopy != engine::runtime::kInvalidEntity) &&
            (std::strcmp(name_of(*world, longCopy), longName) != 0),
        "a full-length name's copy is named differently");
  check(std::strcmp(name_of(*world, longSource), longName) == 0,
        "the full-length source keeps its name");
}

/// A dead handle duplicates nothing and records no history.
void test_invalid() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    g_tests.fail("allocate the world");
    return;
  }
  SessionWorldScope scope(world.get());
  const Entity entity = world->create_scene_object();
  check(world->destroy_entity(entity), "the entity is destroyed");
  const std::size_t aliveBefore = world->alive_entity_count();
  check(engine::editor::execute_entity_duplicate(entity) ==
            engine::runtime::kInvalidEntity,
        "duplicating a dead handle fails");
  check(!engine::editor::editor_session().commandHistory.can_undo(),
        "the failed duplicate records no history");
  check(world->alive_entity_count() == aliveBefore,
        "the failed duplicate creates nothing");
}

} // namespace

/// Runs this executable or test program.
int main() {
  test_single_entity();
  test_subtree();
  test_names();
  test_invalid();
  return g_tests.finish("editor duplicate entity tests");
}
