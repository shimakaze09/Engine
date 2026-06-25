// Declares json types and APIs for the Engine core engine.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace engine::core {

/// Stores json value data used by the engine.
struct JsonValue final {
  /// Enumerates type values used by the engine.
  enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };

  Type type = Type::Null;
  const char *begin = nullptr;
  const char *end = nullptr;
};

/// Owns the json writer behavior and state.
class JsonWriter final {
public:
  static constexpr std::size_t kBufferBytes = 256U * 1024U;
  static constexpr std::size_t kMaxBufferBytes = 16U * 1024U * 1024U;

  JsonWriter() noexcept;
  ~JsonWriter() noexcept;

  JsonWriter(const JsonWriter &) = delete;
  /// Handles operator=.
  JsonWriter &operator=(const JsonWriter &) = delete;
  JsonWriter(JsonWriter &&) = delete;
  /// Handles operator=.
  JsonWriter &operator=(JsonWriter &&) = delete;

  /// Resets this object back to its reusable empty state.
  void reset() noexcept;

  /// Begins the requested operation or profiling range for object.
  void begin_object() noexcept;
  /// Ends the requested operation or profiling range for object.
  void end_object() noexcept;

  /// Begins the requested operation or profiling range for array.
  void begin_array(const char *key) noexcept;
  /// Begins the requested operation or profiling range for array.
  void begin_array() noexcept;
  /// Ends the requested operation or profiling range for array.
  void end_array() noexcept;

  /// Writes key data.
  void write_key(const char *key) noexcept;
  /// Writes float data.
  void write_float(const char *key, float value) noexcept;
  /// Writes uint data.
  void write_uint(const char *key, std::uint32_t value) noexcept;
  /// Writes uint64 data.
  void write_uint64(const char *key, std::uint64_t value) noexcept;
  /// Writes bool data.
  void write_bool(const char *key, bool value) noexcept;
  /// Writes string data.
  void write_string(const char *key, const char *value) noexcept;

  /// Writes float value data.
  void write_float_value(float value) noexcept;
  /// Writes uint value data.
  void write_uint_value(std::uint32_t value) noexcept;
  /// Writes uint64 value data.
  void write_uint64_value(std::uint64_t value) noexcept;
  /// Writes bool value data.
  void write_bool_value(bool value) noexcept;
  /// Writes string value data.
  void write_string_value(const char *value) noexcept;

  /// Handles failed.
  bool failed() const noexcept;
  /// Handles ok.
  bool ok() const noexcept;
  /// Handles result.
  const char *result() const noexcept;
  /// Handles result size.
  std::size_t result_size() const noexcept;

private:
  /// Enumerates container kind values used by the engine.
  enum class ContainerKind : std::uint8_t { Object, Array };

  /// Stores container state data used by the engine.
  struct ContainerState final {
    ContainerKind kind = ContainerKind::Object;
    bool firstElement = true;
    bool expectingValue = false;
  };

  /// Begins the requested operation or profiling range for value.
  bool begin_value() noexcept;
  /// Handles ensure capacity.
  bool ensure_capacity(std::size_t additionalBytes) noexcept;
  /// Handles append char.
  bool append_char(char value) noexcept;
  /// Handles append bytes.
  bool append_bytes(const char *value, std::size_t size) noexcept;
  /// Handles append cstr.
  bool append_cstr(const char *value) noexcept;
  /// Handles append escaped.
  bool append_escaped(const char *value) noexcept;
  /// Handles append float.
  bool append_float(float value) noexcept;
  /// Handles append uint.
  bool append_uint(std::uint32_t value) noexcept;
  /// Handles append uint64.
  bool append_uint64(std::uint64_t value) noexcept;
  /// Pushes an item onto the owning stack or queue for container.
  bool push_container(ContainerKind kind) noexcept;
  /// Pops an item from the owning stack or queue for container.
  bool pop_container(ContainerKind kind) noexcept;

  std::unique_ptr<char[]> m_buffer{};
  std::size_t m_capacity = 0U;
  std::array<ContainerState, 32U> m_stack{};
  std::size_t m_pos = 0U;
  std::size_t m_depth = 0U;
  bool m_failed = false;
};

/// Owns the json parser behavior and state.
class JsonParser final {
/// Parses text into the engine representation.
public:
  /// Parses text into the engine representation.
  bool parse(const char *input, std::size_t length) noexcept;
  /// Handles root.
  const JsonValue *root() const noexcept;

  // Pointer-returning navigation helpers are transient: do not keep returned
  // pointers across additional pointer-returning navigation calls.
  const JsonValue *get_object_field(const JsonValue &object,
                                    const char *fieldName) const noexcept;
  /// Returns the requested value for object field.
  bool get_object_field(const JsonValue &object, const char *fieldName,
                        JsonValue *outValue) const noexcept;

  /// Returns the requested value for array element.
  const JsonValue *get_array_element(const JsonValue &array,
                                     std::size_t index) const noexcept;
  /// Returns the requested value for array element.
  bool get_array_element(const JsonValue &array, std::size_t index,
                         JsonValue *outValue) const noexcept;

  /// Handles array size.
  std::size_t array_size(const JsonValue &array) const noexcept;

  /// Handles as float.
  bool as_float(const JsonValue &value, float *outValue) const noexcept;
  /// Handles as uint.
  bool as_uint(const JsonValue &value, std::uint32_t *outValue) const noexcept;
  /// Handles as uint64.
  bool as_uint64(const JsonValue &value,
                 std::uint64_t *outValue) const noexcept;
  /// Handles as bool.
  bool as_bool(const JsonValue &value, bool *outValue) const noexcept;
  /// Handles as string.
  bool as_string(const JsonValue &value, const char **outBegin,
                 std::size_t *outLength) const noexcept;
  /// Copies a decoded string value into a null-terminated output buffer.
  bool copy_string(const JsonValue &value, char *out,
                   std::size_t outCapacity) const noexcept;

/// Pushes an item onto the owning stack or queue for scratch.
private:
  /// Pushes an item onto the owning stack or queue for scratch.
  const JsonValue *push_scratch(const JsonValue &value) const noexcept;

  const char *m_input = nullptr;
  std::size_t m_length = 0U;
  JsonValue m_root{};
  bool m_hasRoot = false;
  mutable std::array<JsonValue, 1024U> m_scratch{};
  mutable std::size_t m_scratchCursor = 0U;
};

} // namespace engine::core
