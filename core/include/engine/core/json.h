// Declares json types and APIs for the Engine core engine.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace engine::core {

/// Parsed JSON node handle; navigate it through JsonParser accessors.
struct JsonValue final {
  /// Enumerates type values used by the engine.
  enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };

  Type type = Type::Null;
  const char *begin = nullptr;
  const char *end = nullptr;
};

/// Appends JSON into a fixed buffer; failure is sticky (check ok()).
class JsonWriter final {
public:
  static constexpr std::size_t kBufferBytes = 256U * 1024U;
  static constexpr std::size_t kMaxBufferBytes = 16U * 1024U * 1024U;

  JsonWriter() noexcept;
  ~JsonWriter() noexcept;

  JsonWriter(const JsonWriter &) = delete;
  JsonWriter &operator=(const JsonWriter &) = delete;
  JsonWriter(JsonWriter &&) = delete;
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

  /// True once any append overflowed or was malformed (sticky).
  bool failed() const noexcept;
  /// True when the document is complete and no append failed.
  bool ok() const noexcept;
  /// Null-terminated JSON text (valid until reset).
  const char *result() const noexcept;
  /// Byte length of result(), excluding the terminator.
  std::size_t result_size() const noexcept;

private:
  /// Enumerates container kind values used by the engine.
  enum class ContainerKind : std::uint8_t { Object, Array };

  /// Tracks one open object/array while writing (comma placement).
  struct ContainerState final {
    ContainerKind kind = ContainerKind::Object;
    bool firstElement = true;
    bool expectingValue = false;
  };

  /// Begins the requested operation or profiling range for value.
  bool begin_value() noexcept;
  /// Grows usage bookkeeping; false (sticky failure) on overflow.
  bool ensure_capacity(std::size_t additionalBytes) noexcept;
  /// Appends one raw character.
  bool append_char(char value) noexcept;
  /// Appends `size` raw bytes.
  bool append_bytes(const char *value, std::size_t size) noexcept;
  /// Appends a raw null-terminated string (no escaping).
  bool append_cstr(const char *value) noexcept;
  /// Appends a string with JSON escaping applied.
  bool append_escaped(const char *value) noexcept;
  /// Appends a float in round-trip-stable decimal form.
  bool append_float(float value) noexcept;
  /// Appends an unsigned 32-bit integer.
  bool append_uint(std::uint32_t value) noexcept;
  /// Appends an unsigned 64-bit integer.
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

/// Parses JSON into fixed storage; query values via JsonValue handles.
class JsonParser final {
public:
  /// Parses text into the engine representation.
  bool parse(const char *input, std::size_t length) noexcept;
  /// Root value of the last successful parse, or nullptr.
  const JsonValue *root() const noexcept;

  // Pointer-returning navigation helpers are transient: do not keep returned
  // pointers across additional pointer-returning navigation calls.
  const JsonValue *get_object_field(const JsonValue &object,
                                    const char *fieldName) const noexcept;
  /// Finds a field by name in an object; false when missing.
  bool get_object_field(const JsonValue &object, const char *fieldName,
                        JsonValue *outValue) const noexcept;

  /// Element at index, or nullptr out of range.
  const JsonValue *get_array_element(const JsonValue &array,
                                     std::size_t index) const noexcept;
  /// Copies the element at index; false out of range.
  bool get_array_element(const JsonValue &array, std::size_t index,
                         JsonValue *outValue) const noexcept;

  /// Number of elements in an array value (0 for non-arrays).
  std::size_t array_size(const JsonValue &array) const noexcept;

  /// Numeric value as float; false for non-numbers.
  bool as_float(const JsonValue &value, float *outValue) const noexcept;
  /// Numeric value as uint32; false for non-numbers or out of range.
  bool as_uint(const JsonValue &value, std::uint32_t *outValue) const noexcept;
  /// Numeric value as uint64; false for non-numbers or out of range.
  bool as_uint64(const JsonValue &value,
                 std::uint64_t *outValue) const noexcept;
  /// Boolean value; false for non-booleans.
  bool as_bool(const JsonValue &value, bool *outValue) const noexcept;
  /// Unescaped string view (begin + length); false for non-strings.
  bool as_string(const JsonValue &value, const char **outBegin,
                 std::size_t *outLength) const noexcept;
  /// Copies a decoded string value into a null-terminated output buffer.
  bool copy_string(const JsonValue &value, char *out,
                   std::size_t outCapacity) const noexcept;

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
