// Implements the reflection registry. Refused registrations are counted on
// the descriptor and the registry and logged when logging is live; static
// REFLECT_TYPE blocks run before logging initializes, so the counts are the
// durable record and report_reflection_registration_drops surfaces them
// once core initialization has a log to write to.

#include "engine/core/reflect.h"

#include "engine/core/hash.h"
#include "engine/core/logging.h"

#include <cstdio>
#include <cstring>

namespace engine::core {

namespace {

/// Logs one refused registration; a no-op until logging is initialized.
/// Every directive is bounded and the buffer holds the sum of the bounds,
/// so the message is never truncated (and the compiler can prove it).
void log_drop(const char *what, const char *typeName,
              const char *fieldName, const char *reason) noexcept {
  char message[320] = {};
  if (fieldName != nullptr) {
    std::snprintf(message, sizeof(message),
                  "reflection dropped %.8s '%.96s' of type '%.96s': %.64s",
                  what, fieldName,
                  (typeName != nullptr) ? typeName : "<null>", reason);
  } else {
    std::snprintf(message, sizeof(message),
                  "reflection dropped %.8s '%.96s': %.64s", what,
                  (typeName != nullptr) ? typeName : "<null>", reason);
  }
  log_message(LogLevel::Error, "reflect", message);
}

} // namespace

bool TypeDescriptor::add_field(const char *fieldName, std::size_t fieldOffset,
                               std::size_t fieldSize,
                               TypeField::Kind fieldKind,
                               const char *fieldKey) noexcept {
  const char *key = (fieldKey != nullptr) ? fieldKey : fieldName;
  const FieldId id = (key != nullptr) ? fnv1a_32(key) : 0U;
  const char *reason = nullptr;
  if (fieldName == nullptr) {
    reason = "null field name";
  } else if (key[0] == '\0') {
    reason = "empty wire key";
  } else if ((fieldOffset > size) || (fieldSize > (size - fieldOffset))) {
    reason = "field lies outside the type";
  } else if (fieldCount >= fields.size()) {
    reason = "field table is full";
  } else if ((find_field_by_key(key) != nullptr) ||
             (find_field_by_id(id) != nullptr)) {
    // Two fields on one wire key (or one 32-bit id) could never both round
    // trip, so the second is a schema error rather than a silent shadow.
    reason = "duplicate wire key";
  }

  if (reason != nullptr) {
    ++droppedFieldCount;
    log_drop("field", name, fieldName, reason);
    return false;
  }

  TypeField &field = fields[fieldCount++];
  field.name = fieldName;
  field.key = key;
  field.id = id;
  field.offset = fieldOffset;
  field.size = fieldSize;
  field.kind = fieldKind;
  return true;
}

const TypeField *
TypeDescriptor::find_field(const char *fieldName) const noexcept {
  if ((fieldName == nullptr) || (fieldCount == 0U)) {
    return nullptr;
  }

  for (std::size_t i = 0U; i < fieldCount; ++i) {
    const TypeField &field = fields[i];
    if ((field.name != nullptr) && (std::strcmp(field.name, fieldName) == 0)) {
      return &field;
    }
  }

  return nullptr;
}

const TypeField *
TypeDescriptor::find_field_by_key(const char *fieldKey) const noexcept {
  if ((fieldKey == nullptr) || (fieldCount == 0U)) {
    return nullptr;
  }

  for (std::size_t i = 0U; i < fieldCount; ++i) {
    const TypeField &field = fields[i];
    if ((field.key != nullptr) && (std::strcmp(field.key, fieldKey) == 0)) {
      return &field;
    }
  }

  return nullptr;
}

const TypeField *TypeDescriptor::find_field_by_id(FieldId fieldId) const noexcept {
  for (std::size_t i = 0U; i < fieldCount; ++i) {
    if (fields[i].id == fieldId) {
      return &fields[i];
    }
  }

  return nullptr;
}

TypeDescriptor *TypeRegistry::register_type(const char *name,
                                            std::size_t size) noexcept {
  if ((name == nullptr) || (size == 0U)) {
    ++droppedTypeCount;
    log_drop("type", name, nullptr,
             (name == nullptr) ? "null type name" : "zero-sized type");
    return nullptr;
  }

  for (std::size_t i = 0U; i < typeCount; ++i) {
    TypeDescriptor &type = types[i];
    if ((type.name != nullptr) && (std::strcmp(type.name, name) == 0)) {
      return &type;
    }
  }

  if (typeCount >= types.size()) {
    ++droppedTypeCount;
    log_drop("type", name, nullptr, "type registry is full");
    return nullptr;
  }

  TypeDescriptor &type = types[typeCount++];
  type.name = name;
  type.size = size;
  type.fieldCount = 0U;
  type.droppedFieldCount = 0U;
  type.fields.fill(TypeField{});
  return &type;
}

bool TypeRegistry::add_field(TypeDescriptor *descriptor, const char *fieldName,
                             std::size_t fieldOffset, std::size_t fieldSize,
                             TypeField::Kind fieldKind,
                             const char *fieldKey) noexcept {
  if (descriptor == nullptr) {
    // The type itself was refused; its fields can only be counted here.
    ++droppedFieldCount;
    log_drop("field", nullptr, fieldName, "its type was not registered");
    return false;
  }

  if (!descriptor->add_field(fieldName, fieldOffset, fieldSize, fieldKind,
                             fieldKey)) {
    ++droppedFieldCount;
    return false;
  }
  return true;
}

const TypeDescriptor *TypeRegistry::find_type(const char *name) const noexcept {
  if ((name == nullptr) || (typeCount == 0U)) {
    return nullptr;
  }

  for (std::size_t i = 0U; i < typeCount; ++i) {
    const TypeDescriptor &type = types[i];
    if ((type.name != nullptr) && (std::strcmp(type.name, name) == 0)) {
      return &type;
    }
  }

  return nullptr;
}

std::size_t TypeRegistry::type_count() const noexcept { return typeCount; }

const TypeDescriptor *TypeRegistry::type_at(std::size_t index) const noexcept {
  if (index >= typeCount) {
    return nullptr;
  }

  return &types[index];
}

std::size_t TypeRegistry::dropped_type_count() const noexcept {
  return droppedTypeCount;
}

std::size_t TypeRegistry::dropped_field_count() const noexcept {
  return droppedFieldCount;
}

TypeRegistry &global_type_registry() noexcept {
  static TypeRegistry registry{};
  return registry;
}

bool report_reflection_registration_drops() noexcept {
  const TypeRegistry &registry = global_type_registry();
  const std::size_t droppedTypes = registry.dropped_type_count();
  const std::size_t droppedFields = registry.dropped_field_count();
  if ((droppedTypes == 0U) && (droppedFields == 0U)) {
    return true;
  }

  char message[256] = {};
  std::snprintf(message, sizeof(message),
                "reflection registration dropped %zu type(s) and %zu "
                "field(s); the schema is incomplete (registry capacity %zu "
                "types, %zu fields per type)",
                droppedTypes, droppedFields, TypeRegistry::kMaxTypes,
                TypeDescriptor::kMaxFields);
  log_message(LogLevel::Error, "reflect", message);
  return false;
}

} // namespace engine::core
