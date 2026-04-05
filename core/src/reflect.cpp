#include "engine/core/reflect.h"

#include <cstring>

namespace engine::core {

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

TypeDescriptor *TypeRegistry::register_type(const char *name,
                                            std::size_t size) noexcept {
  if ((name == nullptr) || (size == 0U)) {
    return nullptr;
  }

  for (std::size_t i = 0U; i < typeCount; ++i) {
    TypeDescriptor &type = types[i];
    if ((type.name != nullptr) && (std::strcmp(type.name, name) == 0)) {
      return &type;
    }
  }

  if (typeCount >= types.size()) {
    return nullptr;
  }

  TypeDescriptor &type = types[typeCount++];
  type.name = name;
  type.size = size;
  type.fieldCount = 0U;
  type.fields.fill(TypeField{});
  return &type;
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

TypeRegistry &global_type_registry() noexcept {
  static TypeRegistry registry{};
  return registry;
}

} // namespace engine::core
