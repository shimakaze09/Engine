// Verifies placing assets from the Assets panel through the one entry
// point every drop and double-click calls, execute_asset_instantiate, on
// a private asset tree:
// - a prefab dropped at a point becomes an instance with every component
//   the prefab holds, at that point and with the prefab's own scale, as
//   one undo step that one Undo removes and Redo restores under the same
//   persistent id;
// - a prefab dropped on an entity becomes that entity's child, at its
//   origin, unless it holds a dynamic body, which must be a scene root;
// - a cooked mesh and the model it is cooked from become a mesh entity
//   that plays the one animation controller driving the mesh's skeleton,
//   and none when two controllers drive it;
// - a missing prefab, a model with no cooked mesh, a kind that cannot be
//   placed, a parent no longer alive, and any drop during Play are
//   refused with the world and the history unchanged;
// - a prefab's double-click Open places it, as a cooked mesh's does.
// Before this, only a cooked mesh could be placed (as a bare mesh, by a
// Scene view drop), and the editor had no path at all to place a prefab.

#include "editor_asset_index.h"
#include "editor_asset_place.h"
#include "editor_commands.h"
#include "editor_session.h"

#include "engine/content/asset_catalog.h"
#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/editor/editor.h"
#include "engine/renderer/asset_database.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/prefab_serializer.h"
#include "engine/runtime/service_registry.h"
#include "engine/runtime/world.h"

#include "../test_harness.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <system_error>

namespace {

using engine::content::AssetTypeTag;
using engine::editor::AssetPlacePayload;
using engine::editor::editor_session;
using engine::editor::EntitySpawnPlacement;
using engine::runtime::Entity;
using engine::runtime::kInvalidEntity;
using engine::runtime::World;

engine::tests::TestContext g_tests;

bool write_text(const char *path, const char *text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
  return static_cast<bool>(out);
}

AssetPlacePayload payload(const char *virtualPath, AssetTypeTag kind,
                          bool isSource = false) {
  AssetPlacePayload asset{};
  std::snprintf(asset.virtualPath, sizeof(asset.virtualPath), "%s",
                virtualPath);
  asset.kind = kind;
  asset.isSource = isSource;
  return asset;
}

/// Authors assets/lamp.prefab: a named entity with a static collider.
bool author_static_prefab(World &world) {
  const Entity source = world.create_scene_object();
  engine::runtime::NameComponent name{};
  std::snprintf(name.name, sizeof(name.name), "Lamp");
  engine::runtime::Collider collider{};
  collider.halfExtents = engine::math::Vec3(0.2F, 1.0F, 0.2F);
  const bool ok =
      (source != kInvalidEntity) && world.add_name_component(source, name) &&
      world.add_collider(source, collider) &&
      engine::runtime::save_prefab(world, source, "assets/lamp.prefab");
  static_cast<void>(world.destroy_entity(source));
  return ok;
}

/// Authors assets/crate.prefab: a named, scaled box body.
bool author_prefab(World &world) {
  engine::runtime::Transform transform{};
  transform.position = engine::math::Vec3(9.0F, 9.0F, 9.0F);
  transform.scale = engine::math::Vec3(2.0F, 2.0F, 2.0F);
  const Entity source = world.create_scene_object(transform);
  engine::runtime::NameComponent name{};
  std::snprintf(name.name, sizeof(name.name), "Crate");
  engine::runtime::Collider collider{};
  collider.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  engine::runtime::RigidBody body{};
  body.inverseMass = 1.0F;
  const bool ok =
      (source != kInvalidEntity) && world.add_name_component(source, name) &&
      world.add_collider(source, collider) &&
      world.add_rigid_body(source, body) &&
      engine::runtime::save_prefab(world, source, "assets/crate.prefab");
  static_cast<void>(world.destroy_entity(source));
  return ok;
}

/// A skinned mesh with its skeleton and the controller driving it, the
/// glTF it is cooked from, and a model nobody cooked.
bool author_mesh_kit() {
  return write_text("assets/hero.mesh", "cooked") &&
         write_text("assets/hero.gltf", "{}") &&
         write_text("assets/hero.skel", "skeleton") &&
         write_text("assets/hero.animctrl",
                    "{\"skeleton\":\"assets/hero.skel\"}") &&
         write_text("assets/raw.gltf", "{}") &&
         write_text("assets/rock.mesh", "cooked") &&
         write_text("assets/rock.png", "png");
}

std::size_t alive(const World &world) { return world.alive_entity_count(); }

void check_prefab_at_point(World &world) {
  const std::size_t before = alive(world);
  EntitySpawnPlacement placement{};
  placement.hasPosition = true;
  placement.position = engine::math::Vec3(5.0F, 0.0F, 3.0F);
  const Entity placed = engine::editor::execute_asset_instantiate(
      payload("assets/crate.prefab", AssetTypeTag::Prefab), placement);
  g_tests.check(placed != kInvalidEntity, "a dropped prefab is placed");
  g_tests.check(alive(world) == before + 1U,
                "the drop adds exactly one entity");
  engine::runtime::Transform transform{};
  engine::runtime::NameComponent name{};
  engine::runtime::Collider collider{};
  engine::runtime::RigidBody body{};
  g_tests.check(
      world.get_transform(placed, &transform) &&
          (transform.position.x == 5.0F) && (transform.position.y == 0.0F) &&
          (transform.position.z == 3.0F) && (transform.scale.x == 2.0F),
      "it stands at the drop point with the prefab's scale");
  g_tests.check(world.get_name_component(placed, &name) &&
                    (std::strcmp(name.name, "Crate") == 0),
                "it keeps the prefab's name");
  g_tests.check(world.get_collider(placed, &collider) &&
                    world.get_rigid_body(placed, &body) &&
                    (body.inverseMass == 1.0F),
                "it holds every component of the prefab");
  g_tests.check(engine::editor::selected_entity() == placed,
                "the instance becomes the selection");

  const engine::runtime::PersistentId id = world.persistent_id(placed);
  g_tests.check(editor_session().commandHistory.undo() &&
                    (alive(world) == before),
                "one Undo removes the instance");
  g_tests.check(editor_session().commandHistory.redo() &&
                    (alive(world) == before + 1U) &&
                    (world.find_entity_by_persistent_id(id) != kInvalidEntity),
                "Redo restores it under the same persistent id");
}

void check_prefab_under_parent(World &world) {
  engine::runtime::Transform parentTransform{};
  parentTransform.position = engine::math::Vec3(1.0F, 2.0F, 3.0F);
  const Entity parent = world.create_scene_object(parentTransform);
  EntitySpawnPlacement placement{};
  placement.parent = parent;
  const Entity placed = engine::editor::execute_asset_instantiate(
      payload("assets/lamp.prefab", AssetTypeTag::Prefab), placement);
  engine::runtime::Transform transform{};
  g_tests.check(
      (placed != kInvalidEntity) && world.get_transform(placed, &transform) &&
          (transform.parentId == world.persistent_id(parent)) &&
          (transform.position.x == 0.0F) && (transform.position.y == 0.0F) &&
          (transform.position.z == 0.0F),
      "a prefab dropped on an entity becomes its child, at its "
      "origin");
}

void check_meshes(World &world) {
  engine::runtime::AnimationComponent animation{};
  engine::runtime::MeshComponent mesh{};
  const Entity cooked = engine::editor::execute_asset_instantiate(
      payload("assets/hero.mesh", AssetTypeTag::Mesh), EntitySpawnPlacement{});
  g_tests.check((cooked != kInvalidEntity) &&
                    world.get_mesh_component(cooked, &mesh) &&
                    (mesh.meshAssetId != 0U),
                "a cooked mesh is placed with its mesh");
  g_tests.check(
      world.get_animation_component(cooked, &animation) &&
          (std::strcmp(animation.controllerPath, "assets/hero.animctrl") == 0),
      "a skinned mesh plays the controller driving its skeleton");

  const Entity model = engine::editor::execute_asset_instantiate(
      payload("assets/hero.gltf", AssetTypeTag::Mesh, true),
      EntitySpawnPlacement{});
  engine::runtime::MeshComponent modelMesh{};
  engine::runtime::NameComponent name{};
  g_tests.check((model != kInvalidEntity) &&
                    world.get_mesh_component(model, &modelMesh) &&
                    (modelMesh.meshAssetId == mesh.meshAssetId) &&
                    world.get_animation_component(model, &animation),
                "a model places the mesh cooked from it, animated");
  g_tests.check(world.get_name_component(model, &name) &&
                    (std::strncmp(name.name, "hero", 4U) == 0),
                "the model's entity is named after the file");

  const Entity rock = engine::editor::execute_asset_instantiate(
      payload("assets/rock.mesh", AssetTypeTag::Mesh), EntitySpawnPlacement{});
  g_tests.check((rock != kInvalidEntity) &&
                    !world.get_animation_component(rock, &animation),
                "a mesh with no skeleton gets no Animation component");

  // A second controller driving the same skeleton makes the choice a
  // guess, so none is attached.
  g_tests.check(write_text("assets/hero_alt.animctrl",
                           "{\"skeleton\":\"assets/hero.skel\"}") &&
                    engine::editor::rebuild_asset_index(),
                "a second controller is indexed");
  const Entity ambiguous = engine::editor::execute_asset_instantiate(
      payload("assets/hero.mesh", AssetTypeTag::Mesh), EntitySpawnPlacement{});
  g_tests.check((ambiguous != kInvalidEntity) &&
                    !world.get_animation_component(ambiguous, &animation),
                "with two controllers for its skeleton, none is attached");
}

/// Each refusal leaves the world and the history as they were.
void check_refused(World &world, const AssetPlacePayload &asset,
                   const EntitySpawnPlacement &placement, const char *what) {
  const std::size_t before = alive(world);
  const std::uint64_t token = editor_session().commandHistory.current_token();
  const Entity placed =
      engine::editor::execute_asset_instantiate(asset, placement);
  char label[160] = {};
  std::snprintf(label, sizeof(label), "%s is refused, the world unchanged",
                what);
  g_tests.check((placed == kInvalidEntity) && (alive(world) == before) &&
                    (editor_session().commandHistory.current_token() == token),
                label);
}

void check_refusals(World &world) {
  check_refused(world, payload("assets/missing.prefab", AssetTypeTag::Prefab),
                EntitySpawnPlacement{}, "a missing prefab");
  g_tests.check(write_text("assets/broken.prefab", "{ not a prefab"),
                "write a broken prefab");
  check_refused(world, payload("assets/broken.prefab", AssetTypeTag::Prefab),
                EntitySpawnPlacement{}, "an unreadable prefab");
  check_refused(world, payload("assets/raw.gltf", AssetTypeTag::Mesh, true),
                EntitySpawnPlacement{}, "a model with no cooked mesh");
  check_refused(world, payload("assets/rock.png", AssetTypeTag::Texture),
                EntitySpawnPlacement{}, "a texture");

  // A dynamic body must be a scene root, so the World refuses one placed
  // under a parent, and the placement rolls back whole.
  const Entity holder = world.create_scene_object();
  EntitySpawnPlacement under{};
  under.parent = holder;
  check_refused(world, payload("assets/crate.prefab", AssetTypeTag::Prefab),
                under, "a body prefab dropped on an entity");

  const Entity parent = world.create_scene_object();
  static_cast<void>(world.destroy_entity(parent));
  EntitySpawnPlacement gone{};
  gone.parent = parent;
  check_refused(world, payload("assets/crate.prefab", AssetTypeTag::Prefab),
                gone, "a drop on an entity no longer alive");

  editor_session().playState = engine::editor::PlayState::Playing;
  check_refused(world, payload("assets/crate.prefab", AssetTypeTag::Prefab),
                EntitySpawnPlacement{}, "a drop during Play");
  editor_session().playState = engine::editor::PlayState::Stopped;
}

void check_double_click(World &world) {
  g_tests.check(
      engine::editor::resolve_asset_open_action(AssetTypeTag::Prefab, true) ==
          engine::editor::AssetOpenAction::PlaceAsset,
      "a prefab's Open places it");
  engine::editor::AssetIndexEntry entry{};
  std::snprintf(entry.virtualPath, sizeof(entry.virtualPath),
                "assets/crate.prefab");
  std::snprintf(entry.osPath, sizeof(entry.osPath), "assets/crate.prefab");
  std::snprintf(entry.name, sizeof(entry.name), "crate.prefab");
  entry.kind = AssetTypeTag::Prefab;
  entry.isSource = true;
  const std::size_t before = alive(world);
  g_tests.check(engine::editor::execute_asset_open(entry) &&
                    (alive(world) == before + 1U),
                "double-clicking a prefab places an instance");
}

} // namespace

/// Runs this executable or test program.
int main() {
  // A private working directory holds this suite's "assets" root, which
  // the index walks, apart from the trees other suites rewrite in
  // parallel.
  constexpr const char *kWorkDir = "engine_asset_place_test_wd";
  std::error_code ec{};
  const std::filesystem::path previous = std::filesystem::current_path(ec);
  std::filesystem::remove_all(kWorkDir, ec);
  std::filesystem::create_directories(
      std::filesystem::path(kWorkDir) / "assets", ec);
  std::filesystem::current_path(kWorkDir, ec);
  if (ec || !engine::core::initialize_logging() ||
      !engine::core::initialize_vfs() ||
      !engine::core::mount("assets", "assets")) {
    return 2;
  }
  std::unique_ptr<engine::renderer::AssetDatabase> database(
      new (std::nothrow) engine::renderer::AssetDatabase());
  std::unique_ptr<engine::content::AssetCatalog> catalog(
      new (std::nothrow) engine::content::AssetCatalog());
  std::unique_ptr<World> world(new (std::nothrow) World());
  if ((database == nullptr) || (catalog == nullptr) || (world == nullptr)) {
    return 3;
  }
  engine::runtime::EngineAssetDatabaseService service{};
  service.catalog = catalog.get();
  service.database = database.get();
  engine::runtime::set_editor_asset_service(&service);
  engine::editor::editor_set_world(world.get());

  const bool authored = author_prefab(*world) && author_static_prefab(*world) &&
                        author_mesh_kit() &&
                        engine::editor::rebuild_asset_index();
  g_tests.check(authored, "the asset tree is authored and indexed");
  if (authored) {
    check_prefab_at_point(*world);
    check_prefab_under_parent(*world);
    check_meshes(*world);
    check_refusals(*world);
    check_double_click(*world);
  }

  editor_session().commandHistory.clear();
  engine::editor::editor_set_world(nullptr);
  engine::runtime::set_editor_asset_service(nullptr);
  engine::editor::asset_index_reset();
  engine::core::shutdown_vfs();
  engine::core::shutdown_logging();
  std::filesystem::current_path(previous, ec);
  std::filesystem::remove_all(kWorkDir, ec);
  return g_tests.finish("editor_asset_place");
}
