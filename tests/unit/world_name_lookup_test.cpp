// Verifies World name-lookup behavior across add, rename, remove, destroy,
// duplicate names, and heavy churn (regression coverage for the incremental
// tombstone-based lookup that replaced full-table rebuilds).

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/runtime/world.h"

namespace {

using engine::runtime::Entity;
using engine::runtime::kInvalidEntity;
using engine::runtime::NameComponent;
using engine::runtime::World;

/// Builds a NameComponent from a literal.
NameComponent make_name(const char *name) {
  NameComponent component{};
  std::snprintf(component.name, sizeof(component.name), "%s", name);
  return component;
}

/// Creates one entity carrying the given name; kInvalidEntity on failure.
Entity spawn_named(World &world, const char *name) {
  const Entity entity = world.create_entity();
  if (entity == kInvalidEntity) {
    return kInvalidEntity;
  }
  if (!world.add_name_component(entity, make_name(name))) {
    return kInvalidEntity;
  }
  return entity;
}

/// Add, lookup hit, lookup miss, and empty-name rejection.
int test_basic_lookup(World &world) {
  const Entity entity = spawn_named(world, "alpha");
  if (entity == kInvalidEntity) {
    return 1;
  }

  if (world.find_entity_by_name("alpha") != entity) {
    return 2;
  }
  if (world.find_entity_by_name("missing") != kInvalidEntity) {
    return 3;
  }
  if (world.find_entity_by_name("") != kInvalidEntity) {
    return 4;
  }
  if (world.find_entity_by_name(nullptr) != kInvalidEntity) {
    return 5;
  }

  if (!world.destroy_entity(entity)) {
    return 6;
  }
  return 0;
}

/// Renaming an entity must retire the old name and expose the new one.
int test_rename(World &world) {
  const Entity entity = spawn_named(world, "before");
  if (entity == kInvalidEntity) {
    return 10;
  }

  if (!world.add_name_component(entity, make_name("after"))) {
    return 11;
  }

  if (world.find_entity_by_name("before") != kInvalidEntity) {
    return 12;
  }
  if (world.find_entity_by_name("after") != entity) {
    return 13;
  }

  if (!world.destroy_entity(entity)) {
    return 14;
  }
  return 0;
}

/// Removing the name component must retire the lookup entry.
int test_remove(World &world) {
  const Entity entity = spawn_named(world, "removable");
  if (entity == kInvalidEntity) {
    return 20;
  }

  if (!world.remove_name_component(entity)) {
    return 21;
  }
  if (world.find_entity_by_name("removable") != kInvalidEntity) {
    return 22;
  }

  if (!world.destroy_entity(entity)) {
    return 23;
  }
  return 0;
}

/// Destroying a named entity must retire the lookup entry.
int test_destroy(World &world) {
  const Entity entity = spawn_named(world, "doomed");
  if (entity == kInvalidEntity) {
    return 30;
  }

  if (!world.destroy_entity(entity)) {
    return 31;
  }
  if (world.find_entity_by_name("doomed") != kInvalidEntity) {
    return 32;
  }
  return 0;
}

/// Two entities sharing a name: destroying one must keep the other findable.
int test_duplicate_names(World &world) {
  const Entity first = spawn_named(world, "twin");
  const Entity second = spawn_named(world, "twin");
  if ((first == kInvalidEntity) || (second == kInvalidEntity)) {
    return 40;
  }

  const Entity foundBefore = world.find_entity_by_name("twin");
  if ((foundBefore != first) && (foundBefore != second)) {
    return 41;
  }

  if (!world.destroy_entity(first)) {
    return 42;
  }
  if (world.find_entity_by_name("twin") != second) {
    return 43;
  }

  if (!world.destroy_entity(second)) {
    return 44;
  }
  if (world.find_entity_by_name("twin") != kInvalidEntity) {
    return 45;
  }
  return 0;
}

/// Heavy create/name/destroy churn: exceeds the tombstone rebuild threshold
/// (capacity/4) and must leave lookups exact afterward.
int test_churn(World &world) {
  constexpr std::size_t kChurnCycles = World::kNameLookupCapacity / 2U;
  char name[NameComponent::kMaxNameLength + 1U] = {};

  for (std::size_t i = 0U; i < kChurnCycles; ++i) {
    std::snprintf(name, sizeof(name), "churn_%zu", i % 977U);
    const Entity entity = spawn_named(world, name);
    if (entity == kInvalidEntity) {
      return 50;
    }
    if (world.find_entity_by_name(name) != entity) {
      return 51;
    }
    if (!world.destroy_entity(entity)) {
      return 52;
    }
  }

  // After churn every transient name must be gone and fresh names exact.
  if (world.find_entity_by_name("churn_1") != kInvalidEntity) {
    return 53;
  }

  const Entity survivor = spawn_named(world, "survivor");
  if (survivor == kInvalidEntity) {
    return 54;
  }
  if (world.find_entity_by_name("survivor") != survivor) {
    return 55;
  }
  if (!world.destroy_entity(survivor)) {
    return 56;
  }
  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }

  const int results[] = {
      test_basic_lookup(*world), test_rename(*world),
      test_remove(*world),       test_destroy(*world),
      test_duplicate_names(*world), test_churn(*world),
  };

  for (const int result : results) {
    if (result != 0) {
      std::fprintf(stderr, "world_name_lookup_test failed with code %d\n",
                   result);
      return result;
    }
  }

  return 0;
}
