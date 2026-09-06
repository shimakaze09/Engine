// Declares the reflection schema: fixed-capacity type descriptors and the
// process-wide registry that static REFLECT_TYPE blocks populate. The
// tables are bounded by design; a registration the tables cannot hold is
// refused, counted, and logged rather than silently dropped, so a type or
// field that never reaches the schema is observable by tests and at boot.

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
  /// Enumerates kind values used by the engine.
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
  static constexpr std::size_t kMaxFields = 16U;

  const char *name = nullptr;
  std::size_t size = 0U;
  std::array<TypeField, kMaxFields> fields{};
  std::size_t fieldCount = 0U;
  /// Fields refused because the table was full or the field was invalid.
  /// Nonzero means the schema is missing authored fields of this type.
  std::size_t droppedFieldCount = 0U;

  /// Appends a field; false (and counted in droppedFieldCount) when the
  /// table is full, the name is null, or the field lies outside the type.
  bool add_field(const char *fieldName, std::size_t fieldOffset,
                 std::size_t fieldSize, TypeField::Kind fieldKind) noexcept;

  // O(fieldCount) string lookup; migrate to field IDs if this becomes hot.
  const TypeField *find_field(const char *fieldName) const noexcept;

  template <typename T>
  /// Typed pointer to the field inside instance (nullptr on size mismatch).
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
  /// Const-typed pointer to the field inside instance.
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
  /// Types refused because the registry was full or the request was
  /// invalid (null name, zero size). Nonzero means a REFLECT_TYPE block
  /// never reached the schema.
  std::size_t droppedTypeCount = 0U;
  /// Fields refused across every descriptor, plus fields declared for a
  /// type that was itself refused (those have no descriptor to count on).
  std::size_t droppedFieldCount = 0U;

  /// Registers a type by name; mutable descriptor, or nullptr (counted in
  /// droppedTypeCount and logged) when the registry cannot hold it. A
  /// second registration of the same name returns the existing descriptor.
  TypeDescriptor *register_type(const char *name, std::size_t size) noexcept;
  /// Appends a field to a descriptor this registry owns; false (counted in
  /// droppedFieldCount and logged) when the descriptor is null or full.
  bool add_field(TypeDescriptor *descriptor, const char *fieldName,
                 std::size_t fieldOffset, std::size_t fieldSize,
                 TypeField::Kind fieldKind) noexcept;
  // O(typeCount) string lookup; migrate to type IDs if this becomes hot.
  const TypeDescriptor *find_type(const char *name) const noexcept;
  /// Number of registered types.
  std::size_t type_count() const noexcept;
  /// Descriptor at index (registration order); nullptr out of range.
  const TypeDescriptor *type_at(std::size_t index) const noexcept;
  /// Number of refused type registrations.
  std::size_t dropped_type_count() const noexcept;
  /// Number of refused field registrations.
  std::size_t dropped_field_count() const noexcept;
};

/// Process-wide registry populated by static REFLECT_TYPE blocks.
TypeRegistry &global_type_registry() noexcept;

/// Logs an error naming the dropped type and field counts of the global
/// registry when either is nonzero; true when nothing was dropped. Static
/// registration runs before logging exists, so core initialization calls
/// this to surface what those blocks could only count.
bool report_reflection_registration_drops() noexcept;

#define ENGINE_REFLECT_CONCAT_IMPL(lhs, rhs) lhs##rhs
#define ENGINE_REFLECT_CONCAT(lhs, rhs) ENGINE_REFLECT_CONCAT_IMPL(lhs, rhs)

// Helper macros to register one type and its fields at static init time.
// A field declared for a type the registry refused still counts as
// dropped, so the registry's totals cover every REFLECT_FIELD line.
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
  static_cast<void>(::engine::core::global_type_registry().add_field(          \
      desc, #FieldName, offsetof(T, FieldName),                                \
      sizeof(decltype(T::FieldName)),                                          \
      ::engine::core::TypeField::Kind::KindEnum));

#define REFLECT_END()                                                          \
  return true;                                                                 \
  }                                                                            \
  ();                                                                          \
  }

} // namespace engine::core
