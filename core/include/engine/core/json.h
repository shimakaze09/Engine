#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace engine::core {

struct JsonValue final {
  enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };

  Type type = Type::Null;
  const char *begin = nullptr;
  const char *end = nullptr;
};

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

  void reset() noexcept;

  void begin_object() noexcept;
  void end_object() noexcept;

  void begin_array(const char *key) noexcept;
  void begin_array() noexcept;
  void end_array() noexcept;

  void write_key(const char *key) noexcept;
  void write_float(const char *key, float value) noexcept;
  void write_uint(const char *key, std::uint32_t value) noexcept;
  void write_bool(const char *key, bool value) noexcept;
  void write_string(const char *key, const char *value) noexcept;

  void write_float_value(float value) noexcept;
  void write_uint_value(std::uint32_t value) noexcept;
  void write_bool_value(bool value) noexcept;
  void write_string_value(const char *value) noexcept;

  bool failed() const noexcept;
  bool ok() const noexcept;
  const char *result() const noexcept;
  std::size_t result_size() const noexcept;

private:
  enum class ContainerKind : std::uint8_t { Object, Array };

  struct ContainerState final {
    ContainerKind kind = ContainerKind::Object;
    bool firstElement = true;
    bool expectingValue = false;
  };

  bool begin_value() noexcept;
  bool ensure_capacity(std::size_t additionalBytes) noexcept;
  bool append_char(char value) noexcept;
  bool append_bytes(const char *value, std::size_t size) noexcept;
  bool append_cstr(const char *value) noexcept;
  bool append_escaped(const char *value) noexcept;
  bool append_float(float value) noexcept;
  bool append_uint(std::uint32_t value) noexcept;
  bool push_container(ContainerKind kind) noexcept;
  bool pop_container(ContainerKind kind) noexcept;

  std::unique_ptr<char[]> m_buffer{};
  std::size_t m_capacity = 0U;
  std::array<ContainerState, 32U> m_stack{};
  std::size_t m_pos = 0U;
  std::size_t m_depth = 0U;
  bool m_failed = false;
};

class JsonParser final {
public:
  bool parse(const char *input, std::size_t length) noexcept;
  const JsonValue *root() const noexcept;

  // Pointer-returning navigation helpers are transient: do not keep returned
  // pointers across additional pointer-returning navigation calls.
  const JsonValue *get_object_field(const JsonValue &object,
                                    const char *fieldName) const noexcept;
  bool get_object_field(const JsonValue &object,
                        const char *fieldName,
                        JsonValue *outValue) const noexcept;

  const JsonValue *get_array_element(const JsonValue &array,
                                     std::size_t index) const noexcept;
  bool get_array_element(const JsonValue &array,
                         std::size_t index,
                         JsonValue *outValue) const noexcept;

  std::size_t array_size(const JsonValue &array) const noexcept;

  bool as_float(const JsonValue &value, float *outValue) const noexcept;
  bool as_uint(const JsonValue &value, std::uint32_t *outValue) const noexcept;
  bool as_bool(const JsonValue &value, bool *outValue) const noexcept;
  bool as_string(const JsonValue &value,
                 const char **outBegin,
                 std::size_t *outLength) const noexcept;

private:
  const JsonValue *push_scratch(const JsonValue &value) const noexcept;

  const char *m_input = nullptr;
  std::size_t m_length = 0U;
  JsonValue m_root{};
  bool m_hasRoot = false;
  mutable std::array<JsonValue, 1024U> m_scratch{};
  mutable std::size_t m_scratchCursor = 0U;
};

} // namespace engine::core
