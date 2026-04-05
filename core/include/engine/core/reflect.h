#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace engine::core {

// TypeField describes one field of a reflected struct.
struct TypeField final {
  const char *name = nullptr;
  std::size_t offset = 0U;
  std::size_t size = 0U;
  enum class Kind : std::uint8_t {
    Float,
    Int32,
    Uint32,
    Bool,
    Vec2,
    Vec3,
    Vec4,
    Quat,
  } kind = Kind::Float;
};

// TypeDescriptor holds all reflected fields for one type.
struct TypeDescriptor final {
  const char *name = nullptr;
  std::size_t size = 0U;
  std::array<TypeField, 16U> fields{};
  std::size_t fieldCount = 0U;

  // O(fieldCount) string lookup; migrate to field IDs if this becomes hot.
  const TypeField *find_field(const char *fieldName) const noexcept;

  template <typename T>
  T *field_ptr(void *instance, const TypeField &field) const noexcept {
    if (instance == nullptr) {
      return nullptr;
    }

    if ((field.offset > size) || (field.size > (size - field.offset))
        || (sizeof(T) > field.size)) {
      return nullptr;
    }

    return reinterpret_cast<T *>(static_cast<std::byte *>(instance)
                                 + field.offset);
  }

  template <typename T>
  const T *field_ptr(const void *instance,
                     const TypeField &field) const noexcept {
    if (instance == nullptr) {
      return nullptr;
    }

    if ((field.offset > size) || (field.size > (size - field.offset))
        || (sizeof(T) > field.size)) {
      return nullptr;
    }

    return reinterpret_cast<const T *>(static_cast<const std::byte *>(instance)
                                       + field.offset);
  }
};

// TypeRegistry holds all registered types.
struct TypeRegistry final {
  static constexpr std::size_t kMaxTypes = 64U;
  std::array<TypeDescriptor, kMaxTypes> types{};
  std::size_t typeCount = 0U;

  TypeDescriptor *register_type(const char *name, std::size_t size) noexcept;
  // O(typeCount) string lookup; migrate to type IDs if this becomes hot.
  const TypeDescriptor *find_type(const char *name) const noexcept;
  std::size_t type_count() const noexcept;
  const TypeDescriptor *type_at(std::size_t index) const noexcept;
};

TypeRegistry &global_type_registry() noexcept;

#define ENGINE_REFLECT_CONCAT_IMPL(lhs, rhs) lhs##rhs
#define ENGINE_REFLECT_CONCAT(lhs, rhs) ENGINE_REFLECT_CONCAT_IMPL(lhs, rhs)

// Helper macros to register one type and its fields at static init time.
#define REFLECT_TYPE(TypeName)                                                 \
  namespace {                                                                  \
  [[maybe_unused]] const bool ENGINE_REFLECT_CONCAT(                           \
      g_reflectType_, __COUNTER__) = []() noexcept {                                                          \
        using T = TypeName;                                                    \
        const char *typeName = #TypeName;                                      \
        ::engine::core::TypeDescriptor *desc =                                 \
            ::engine::core::global_type_registry().register_type(typeName,     \
                                                                  sizeof(T));

#define REFLECT_FIELD(FieldName, KindEnum)                                     \
  if ((desc != nullptr) && (desc->fieldCount < desc->fields.size())) {         \
    ::engine::core::TypeField &f = desc->fields[desc->fieldCount++];           \
    f.name = #FieldName;                                                       \
    f.offset = offsetof(T, FieldName);                                         \
    f.size = sizeof(decltype(T::FieldName));                                   \
    f.kind = ::engine::core::TypeField::Kind::KindEnum;                        \
  }

#define REFLECT_END()                                                          \
  return true;                                                                 \
  }                                                                            \
  ();                                                                          \
  }

} // namespace engine::core
