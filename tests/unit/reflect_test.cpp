// Verifies the reflection schema: the runtime's registered component
// descriptors resolve their fields to the right offsets, a locally registered
// type behaves the same, and a registration the fixed tables cannot hold is
// refused, counted, and reported at core initialization instead of being
// silently dropped (65th type, 17th field, field outside its type, field of
// a refused type).

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "engine/core/logging.h"
#include "engine/core/reflect.h"
#include "engine/runtime/reflect_types.h"
#include "engine/runtime/world.h"

namespace {

struct LocalReflect final {
  std::int32_t id = 0;
  bool enabled = false;
  float weight = 0.0F;
};

/// One more field than a descriptor holds, registered through the
/// production REFLECT_TYPE/REFLECT_FIELD macros against the global registry.
struct SeventeenFields final {
  float f00 = 0.0F;
  float f01 = 0.0F;
  float f02 = 0.0F;
  float f03 = 0.0F;
  float f04 = 0.0F;
  float f05 = 0.0F;
  float f06 = 0.0F;
  float f07 = 0.0F;
  float f08 = 0.0F;
  float f09 = 0.0F;
  float f10 = 0.0F;
  float f11 = 0.0F;
  float f12 = 0.0F;
  float f13 = 0.0F;
  float f14 = 0.0F;
  float f15 = 0.0F;
  float f16 = 0.0F;
};

/// Counts the reflect-channel errors the boot report emits.
struct ReportTally final {
  int reflectErrors = 0;
};

void tally_reflect_errors(engine::core::LogLevel level, const char *channel,
                          const char *, void *userData) noexcept {
  auto *tally = static_cast<ReportTally *>(userData);
  if ((tally != nullptr) && (level == engine::core::LogLevel::Error) &&
      (channel != nullptr) && (std::strcmp(channel, "reflect") == 0)) {
    ++tally->reflectErrors;
  }
}

// Static storage: descriptors keep the name pointer, so the names must
// outlive the registry they are registered into.
char g_overflowTypeNames[engine::core::TypeRegistry::kMaxTypes + 1U][8] = {};

/// The 65th type is refused and counted; the 64 before it all register.
int check_type_overflow() noexcept {
  using engine::core::TypeRegistry;
  static TypeRegistry registry{};
  for (std::size_t i = 0U; i <= TypeRegistry::kMaxTypes; ++i) {
    std::snprintf(g_overflowTypeNames[i], sizeof(g_overflowTypeNames[i]),
                  "T%zu", i);
  }

  for (std::size_t i = 0U; i < TypeRegistry::kMaxTypes; ++i) {
    if (registry.register_type(g_overflowTypeNames[i], 4U) == nullptr) {
      return 20;
    }
  }
  if ((registry.type_count() != TypeRegistry::kMaxTypes) ||
      (registry.dropped_type_count() != 0U)) {
    return 21;
  }

  if (registry.register_type(g_overflowTypeNames[TypeRegistry::kMaxTypes],
                             4U) != nullptr) {
    return 22;
  }
  if ((registry.dropped_type_count() != 1U) ||
      (registry.type_count() != TypeRegistry::kMaxTypes) ||
      (registry.find_type(g_overflowTypeNames[TypeRegistry::kMaxTypes]) !=
       nullptr)) {
    return 23;
  }

  // Re-registering an existing name is not a drop: it returns the
  // descriptor already held.
  if ((registry.register_type(g_overflowTypeNames[0], 4U) !=
       registry.type_at(0)) ||
      (registry.dropped_type_count() != 1U)) {
    return 24;
  }

  // A field declared for the refused type has no descriptor to land on and
  // is counted on the registry.
  if (registry.add_field(nullptr, "orphan", 0U, 4U,
                         engine::core::TypeField::Kind::Float) ||
      (registry.dropped_field_count() != 1U)) {
    return 25;
  }
  return 0;
}

/// The 17th field of one type is refused and counted on both the
/// descriptor and its registry; a field outside the type is refused too.
int check_field_overflow() noexcept {
  using engine::core::TypeDescriptor;
  using engine::core::TypeField;
  static engine::core::TypeRegistry registry{};
  TypeDescriptor *desc =
      registry.register_type("Wide", sizeof(float) * (TypeDescriptor::kMaxFields + 1U));
  if (desc == nullptr) {
    return 30;
  }

  static char names[TypeDescriptor::kMaxFields + 1U][8] = {};
  for (std::size_t i = 0U; i <= TypeDescriptor::kMaxFields; ++i) {
    std::snprintf(names[i], sizeof(names[i]), "f%zu", i);
  }

  for (std::size_t i = 0U; i < TypeDescriptor::kMaxFields; ++i) {
    if (!registry.add_field(desc, names[i], sizeof(float) * i, sizeof(float),
                            TypeField::Kind::Float)) {
      return 31;
    }
  }
  if ((desc->fieldCount != TypeDescriptor::kMaxFields) ||
      (desc->droppedFieldCount != 0U) ||
      (registry.dropped_field_count() != 0U)) {
    return 32;
  }

  if (registry.add_field(desc, names[TypeDescriptor::kMaxFields],
                         sizeof(float) * TypeDescriptor::kMaxFields,
                         sizeof(float), TypeField::Kind::Float)) {
    return 33;
  }
  if ((desc->fieldCount != TypeDescriptor::kMaxFields) ||
      (desc->droppedFieldCount != 1U) ||
      (registry.dropped_field_count() != 1U) ||
      (desc->find_field(names[TypeDescriptor::kMaxFields]) != nullptr)) {
    return 34;
  }

  // A field that does not fit inside the type is a schema error, not a
  // capacity one, and is refused on a descriptor with room.
  TypeDescriptor *narrow = registry.register_type("Narrow", sizeof(float));
  if ((narrow == nullptr) ||
      narrow->add_field("beyond", sizeof(float), sizeof(float),
                        TypeField::Kind::Float) ||
      (narrow->fieldCount != 0U) || (narrow->droppedFieldCount != 1U)) {
    return 35;
  }
  return 0;
}

/// The production macro path: SeventeenFields lands in the global registry
/// with 16 fields and one recorded drop, and the boot-time report turns
/// that count into a logged error once logging exists.
int check_macro_overflow_is_reported() noexcept {
  engine::core::TypeRegistry &registry = engine::core::global_type_registry();
  const engine::core::TypeDescriptor *wide =
      registry.find_type("SeventeenFields");
  if (wide == nullptr) {
    return 40;
  }
  if ((wide->fieldCount != engine::core::TypeDescriptor::kMaxFields) ||
      (wide->droppedFieldCount != 1U) || (wide->find_field("f16") != nullptr) ||
      (wide->find_field("f15") == nullptr)) {
    return 41;
  }
  if ((registry.dropped_field_count() != 1U) ||
      (registry.dropped_type_count() != 0U)) {
    return 42;
  }

  if (!engine::core::initialize_logging()) {
    return 43;
  }
  ReportTally tally{};
  if (!engine::core::log_register_sink(&tally_reflect_errors, &tally)) {
    engine::core::shutdown_logging();
    return 44;
  }
  const bool clean = engine::core::report_reflection_registration_drops();
  engine::core::log_unregister_sink(&tally_reflect_errors, &tally);
  engine::core::shutdown_logging();
  if (clean || (tally.reflectErrors != 1)) {
    return 45;
  }
  return 0;
}

} // namespace

REFLECT_TYPE(SeventeenFields)
REFLECT_FIELD(f00, Float)
REFLECT_FIELD(f01, Float)
REFLECT_FIELD(f02, Float)
REFLECT_FIELD(f03, Float)
REFLECT_FIELD(f04, Float)
REFLECT_FIELD(f05, Float)
REFLECT_FIELD(f06, Float)
REFLECT_FIELD(f07, Float)
REFLECT_FIELD(f08, Float)
REFLECT_FIELD(f09, Float)
REFLECT_FIELD(f10, Float)
REFLECT_FIELD(f11, Float)
REFLECT_FIELD(f12, Float)
REFLECT_FIELD(f13, Float)
REFLECT_FIELD(f14, Float)
REFLECT_FIELD(f15, Float)
REFLECT_FIELD(f16, Float)
REFLECT_END()

/// Runs this executable or test program.
int main() {
  if (const int rc = check_type_overflow(); rc != 0) {
    return rc;
  }
  if (const int rc = check_field_overflow(); rc != 0) {
    return rc;
  }
  if (const int rc = check_macro_overflow_is_reported(); rc != 0) {
    return rc;
  }

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
