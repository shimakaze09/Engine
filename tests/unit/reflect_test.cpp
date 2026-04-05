#include <cstddef>
#include <cstdint>

#include "engine/core/reflect.h"
#include "engine/runtime/reflect_types.h"
#include "engine/runtime/world.h"

namespace {

struct LocalReflect final {
  std::int32_t id = 0;
  bool enabled = false;
  float weight = 0.0F;
};

} // namespace

int main() {
  // Explicitly call the anchor symbol so static-library linking pulls
  // runtime reflection registrations into this test binary.
  engine::runtime::ensure_runtime_reflection_registered();

  engine::core::TypeRegistry &registry = engine::core::global_type_registry();

  const engine::core::TypeDescriptor *transformDesc =
      registry.find_type("engine::runtime::Transform");
  if (transformDesc == nullptr) {
    return 1;
  }

  if (transformDesc->fieldCount != 4U) {
    return 2;
  }

  const engine::core::TypeField *positionField =
      transformDesc->find_field("position");
  if (positionField == nullptr) {
    return 3;
  }

  if (positionField->offset != offsetof(engine::runtime::Transform, position)) {
    return 4;
  }

  engine::runtime::Transform transform{};
  engine::math::Vec3 *positionPtr =
      transformDesc->field_ptr<engine::math::Vec3>(&transform, *positionField);
  if (positionPtr != &transform.position) {
    return 5;
  }

  const engine::core::TypeField *parentIdField =
      transformDesc->find_field("parentId");
  if (parentIdField == nullptr) {
    return 15;
  }

  if (parentIdField->offset != offsetof(engine::runtime::Transform, parentId)) {
    return 16;
  }

  engine::core::TypeDescriptor *localDesc =
      registry.register_type("LocalReflect", sizeof(LocalReflect));
  if (localDesc == nullptr) {
    return 6;
  }

  if (localDesc->fieldCount == 0U) {
    localDesc->fields[0].name = "id";
    localDesc->fields[0].offset = offsetof(LocalReflect, id);
    localDesc->fields[0].size = sizeof(decltype(LocalReflect::id));
    localDesc->fields[0].kind = engine::core::TypeField::Kind::Int32;

    localDesc->fields[1].name = "enabled";
    localDesc->fields[1].offset = offsetof(LocalReflect, enabled);
    localDesc->fields[1].size = sizeof(decltype(LocalReflect::enabled));
    localDesc->fields[1].kind = engine::core::TypeField::Kind::Bool;

    localDesc->fields[2].name = "weight";
    localDesc->fields[2].offset = offsetof(LocalReflect, weight);
    localDesc->fields[2].size = sizeof(decltype(LocalReflect::weight));
    localDesc->fields[2].kind = engine::core::TypeField::Kind::Float;

    localDesc->fieldCount = 3U;
  }

  const engine::core::TypeDescriptor *localFound =
      registry.find_type("LocalReflect");
  if (localFound == nullptr) {
    return 7;
  }

  if (localFound->fieldCount != 3U) {
    return 8;
  }

  const engine::core::TypeField *enabledField =
      localFound->find_field("enabled");
  if (enabledField == nullptr) {
    return 9;
  }

  if (enabledField->offset != offsetof(LocalReflect, enabled)) {
    return 10;
  }

  LocalReflect local{};
  const bool *enabledPtr = localFound->field_ptr<bool>(&local, *enabledField);
  if (enabledPtr != &local.enabled) {
    return 11;
  }

  const engine::core::TypeField *weightField = localFound->find_field("weight");
  if (weightField == nullptr) {
    return 12;
  }

  float *weightPtr = localDesc->field_ptr<float>(&local, *weightField);
  if (weightPtr == nullptr) {
    return 13;
  }

  *weightPtr = 3.5F;
  if (local.weight != 3.5F) {
    return 14;
  }

  return 0;
}
