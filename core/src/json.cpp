#include "engine/core/json.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace engine::core {

namespace {

constexpr bool is_whitespace(char value) noexcept {
  return (value == ' ') || (value == '\t') || (value == '\n') ||
         (value == '\r');
}

constexpr bool is_digit(char value) noexcept {
  return (value >= '0') && (value <= '9');
}

constexpr bool is_hex_digit(char value) noexcept {
  return ((value >= '0') && (value <= '9')) ||
         ((value >= 'a') && (value <= 'f')) ||
         ((value >= 'A') && (value <= 'F'));
}

void skip_whitespace(const char *&cursor, const char *end) noexcept {
  while ((cursor < end) && is_whitespace(*cursor)) {
    ++cursor;
  }
}

bool parse_string_token(const char *&cursor, const char *end,
                        const char **outBegin, const char **outEnd) noexcept {
  if ((outBegin == nullptr) || (outEnd == nullptr)) {
    return false;
  }

  if ((cursor >= end) || (*cursor != '"')) {
    return false;
  }

  ++cursor;
  const char *stringBegin = cursor;

  while (cursor < end) {
    const char ch = *cursor;
    if (ch == '"') {
      *outBegin = stringBegin;
      *outEnd = cursor;
      ++cursor;
      return true;
    }

    if (ch == '\\') {
      ++cursor;
      if (cursor >= end) {
        return false;
      }

      if (*cursor == 'u') {
        for (std::size_t i = 0U; i < 4U; ++i) {
          ++cursor;
          if ((cursor >= end) || !is_hex_digit(*cursor)) {
            return false;
          }
        }
      }

      ++cursor;
      continue;
    }

    ++cursor;
  }

  return false;
}

bool parse_number_token(const char *&cursor, const char *end,
                        const char **outBegin, const char **outEnd) noexcept {
  if ((outBegin == nullptr) || (outEnd == nullptr) || (cursor >= end)) {
    return false;
  }

  const char *numberBegin = cursor;

  if (*cursor == '-') {
    ++cursor;
    if (cursor >= end) {
      return false;
    }
  }

  if (*cursor == '0') {
    ++cursor;
  } else {
    if (!is_digit(*cursor)) {
      return false;
    }

    while ((cursor < end) && is_digit(*cursor)) {
      ++cursor;
    }
  }

  if ((cursor < end) && (*cursor == '.')) {
    ++cursor;
    if ((cursor >= end) || !is_digit(*cursor)) {
      return false;
    }

    while ((cursor < end) && is_digit(*cursor)) {
      ++cursor;
    }
  }

  if ((cursor < end) && ((*cursor == 'e') || (*cursor == 'E'))) {
    ++cursor;
    if ((cursor < end) && ((*cursor == '+') || (*cursor == '-'))) {
      ++cursor;
    }

    if ((cursor >= end) || !is_digit(*cursor)) {
      return false;
    }

    while ((cursor < end) && is_digit(*cursor)) {
      ++cursor;
    }
  }

  *outBegin = numberBegin;
  *outEnd = cursor;
  return true;
}

bool parse_value(const char *&cursor, const char *end,
                 JsonValue *outValue) noexcept;

bool parse_array(const char *&cursor, const char *end,
                 JsonValue *outValue) noexcept {
  if ((outValue == nullptr) || (cursor >= end) || (*cursor != '[')) {
    return false;
  }

  const char *arrayBegin = cursor;
  ++cursor;
  skip_whitespace(cursor, end);

  if ((cursor < end) && (*cursor == ']')) {
    ++cursor;
    outValue->type = JsonValue::Type::Array;
    outValue->begin = arrayBegin;
    outValue->end = cursor;
    return true;
  }

  while (cursor < end) {
    JsonValue ignored{};
    if (!parse_value(cursor, end, &ignored)) {
      return false;
    }

    skip_whitespace(cursor, end);
    if (cursor >= end) {
      return false;
    }

    if (*cursor == ',') {
      ++cursor;
      skip_whitespace(cursor, end);
      continue;
    }

    if (*cursor == ']') {
      ++cursor;
      outValue->type = JsonValue::Type::Array;
      outValue->begin = arrayBegin;
      outValue->end = cursor;
      return true;
    }

    return false;
  }

  return false;
}

bool parse_object(const char *&cursor, const char *end,
                  JsonValue *outValue) noexcept {
  if ((outValue == nullptr) || (cursor >= end) || (*cursor != '{')) {
    return false;
  }

  const char *objectBegin = cursor;
  ++cursor;
  skip_whitespace(cursor, end);

  if ((cursor < end) && (*cursor == '}')) {
    ++cursor;
    outValue->type = JsonValue::Type::Object;
    outValue->begin = objectBegin;
    outValue->end = cursor;
    return true;
  }

  while (cursor < end) {
    const char *keyBegin = nullptr;
    const char *keyEnd = nullptr;
    if (!parse_string_token(cursor, end, &keyBegin, &keyEnd)) {
      return false;
    }

    static_cast<void>(keyBegin);
    static_cast<void>(keyEnd);

    skip_whitespace(cursor, end);
    if ((cursor >= end) || (*cursor != ':')) {
      return false;
    }

    ++cursor;

    JsonValue ignored{};
    if (!parse_value(cursor, end, &ignored)) {
      return false;
    }

    skip_whitespace(cursor, end);
    if (cursor >= end) {
      return false;
    }

    if (*cursor == ',') {
      ++cursor;
      skip_whitespace(cursor, end);
      continue;
    }

    if (*cursor == '}') {
      ++cursor;
      outValue->type = JsonValue::Type::Object;
      outValue->begin = objectBegin;
      outValue->end = cursor;
      return true;
    }

    return false;
  }

  return false;
}

bool parse_value(const char *&cursor, const char *end,
                 JsonValue *outValue) noexcept {
  if (outValue == nullptr) {
    return false;
  }

  skip_whitespace(cursor, end);
  if (cursor >= end) {
    return false;
  }

  switch (*cursor) {
  case '{':
    return parse_object(cursor, end, outValue);
  case '[':
    return parse_array(cursor, end, outValue);
  case '"': {
    const char *stringBegin = nullptr;
    const char *stringEnd = nullptr;
    if (!parse_string_token(cursor, end, &stringBegin, &stringEnd)) {
      return false;
    }

    outValue->type = JsonValue::Type::String;
    outValue->begin = stringBegin;
    outValue->end = stringEnd;
    return true;
  }
  case 't':
    if ((end - cursor) < 4 || (std::memcmp(cursor, "true", 4U) != 0)) {
      return false;
    }
    outValue->type = JsonValue::Type::Bool;
    outValue->begin = cursor;
    cursor += 4;
    outValue->end = cursor;
    return true;
  case 'f':
    if ((end - cursor) < 5 || (std::memcmp(cursor, "false", 5U) != 0)) {
      return false;
    }
    outValue->type = JsonValue::Type::Bool;
    outValue->begin = cursor;
    cursor += 5;
    outValue->end = cursor;
    return true;
  case 'n':
    if ((end - cursor) < 4 || (std::memcmp(cursor, "null", 4U) != 0)) {
      return false;
    }
    outValue->type = JsonValue::Type::Null;
    outValue->begin = cursor;
    cursor += 4;
    outValue->end = cursor;
    return true;
  default:
    break;
  }

  const char *numberBegin = nullptr;
  const char *numberEnd = nullptr;
  if (!parse_number_token(cursor, end, &numberBegin, &numberEnd)) {
    return false;
  }

  outValue->type = JsonValue::Type::Number;
  outValue->begin = numberBegin;
  outValue->end = numberEnd;
  return true;
}

bool token_equals(const char *tokenBegin, const char *tokenEnd,
                  const char *text) noexcept {
  if ((tokenBegin == nullptr) || (tokenEnd == nullptr) || (text == nullptr)) {
    return false;
  }

  const std::size_t tokenLength =
      static_cast<std::size_t>(tokenEnd - tokenBegin);
  const std::size_t textLength = std::strlen(text);
  if (tokenLength != textLength) {
    return false;
  }

  return std::memcmp(tokenBegin, text, tokenLength) == 0;
}

} // namespace

JsonWriter::JsonWriter() noexcept { reset(); }

JsonWriter::~JsonWriter() noexcept = default;

void JsonWriter::reset() noexcept {
  m_pos = 0U;
  m_depth = 0U;
  m_failed = false;
  m_stack.fill(ContainerState{});

  if (!ensure_capacity(0U)) {
    m_failed = true;
    return;
  }

  m_buffer[0] = '\0';
}

bool JsonWriter::begin_value() noexcept {
  if (m_failed) {
    return false;
  }

  if (m_depth == 0U) {
    if (m_pos != 0U) {
      m_failed = true;
      return false;
    }

    return true;
  }

  ContainerState &state = m_stack[m_depth - 1U];
  if (state.kind == ContainerKind::Object) {
    if (!state.expectingValue) {
      m_failed = true;
      return false;
    }

    state.expectingValue = false;
    return true;
  }

  if (!state.firstElement) {
    return append_char(',');
  }

  state.firstElement = false;
  return true;
}

bool JsonWriter::append_char(char value) noexcept {
  if (m_failed) {
    return false;
  }

  if (!ensure_capacity(1U)) {
    return false;
  }

  m_buffer[m_pos] = value;
  ++m_pos;
  m_buffer[m_pos] = '\0';
  return true;
}

bool JsonWriter::append_bytes(const char *value, std::size_t size) noexcept {
  if (m_failed || (value == nullptr)) {
    return false;
  }

  if (!ensure_capacity(size)) {
    return false;
  }

  std::memcpy(m_buffer.get() + m_pos, value, size);
  m_pos += size;
  m_buffer[m_pos] = '\0';
  return true;
}

bool JsonWriter::ensure_capacity(std::size_t additionalBytes) noexcept {
  if (m_failed) {
    return false;
  }

  if ((additionalBytes > kMaxBufferBytes) || (m_pos > kMaxBufferBytes) ||
      ((kMaxBufferBytes - m_pos) <= additionalBytes)) {
    m_failed = true;
    return false;
  }

  const std::size_t required = m_pos + additionalBytes + 1U;
  if ((m_buffer != nullptr) && (required <= m_capacity)) {
    return true;
  }

  std::size_t newCapacity = m_capacity;
  if (newCapacity == 0U) {
    newCapacity = kBufferBytes;
  }

  while (newCapacity < required) {
    if (newCapacity >= kMaxBufferBytes) {
      break;
    }

    const std::size_t doubled = newCapacity * 2U;
    if ((doubled <= newCapacity) || (doubled > kMaxBufferBytes)) {
      newCapacity = kMaxBufferBytes;
    } else {
      newCapacity = doubled;
    }
  }

  if ((newCapacity < required) || (newCapacity > kMaxBufferBytes)) {
    m_failed = true;
    return false;
  }

  std::unique_ptr<char[]> newBuffer(new (std::nothrow) char[newCapacity]);
  if (newBuffer == nullptr) {
    m_failed = true;
    return false;
  }

  if ((m_buffer != nullptr) && (m_pos > 0U)) {
    std::memcpy(newBuffer.get(), m_buffer.get(), m_pos);
  }
  newBuffer[m_pos] = '\0';

  m_buffer.swap(newBuffer);
  m_capacity = newCapacity;
  return true;
}

bool JsonWriter::append_cstr(const char *value) noexcept {
  if (value == nullptr) {
    m_failed = true;
    return false;
  }

  return append_bytes(value, std::strlen(value));
}

bool JsonWriter::append_escaped(const char *value) noexcept {
  if (value == nullptr) {
    value = "";
  }

  if (!append_char('"')) {
    return false;
  }

  for (const char *cursor = value; *cursor != '\0'; ++cursor) {
    const unsigned char ch = static_cast<unsigned char>(*cursor);
    switch (ch) {
    case '"':
      if (!append_cstr("\\\"")) {
        return false;
      }
      break;
    case '\\':
      if (!append_cstr("\\\\")) {
        return false;
      }
      break;
    case '\n':
      if (!append_cstr("\\n")) {
        return false;
      }
      break;
    case '\r':
      if (!append_cstr("\\r")) {
        return false;
      }
      break;
    case '\t':
      if (!append_cstr("\\t")) {
        return false;
      }
      break;
    default:
      if (ch < 0x20U) {
        char escapeBuffer[7] = {};
        const int written = std::snprintf(escapeBuffer, sizeof(escapeBuffer),
                                          "\\u%04X", static_cast<unsigned>(ch));
        if ((written != 6) || !append_bytes(escapeBuffer, 6U)) {
          m_failed = true;
          return false;
        }
      } else {
        if (!append_char(static_cast<char>(ch))) {
          return false;
        }
      }
      break;
    }
  }

  return append_char('"');
}

bool JsonWriter::append_float(float value) noexcept {
  if (!std::isfinite(value)) {
    m_failed = true;
    return false;
  }

  char numberBuffer[32] = {};
  const int written = std::snprintf(numberBuffer, sizeof(numberBuffer), "%.9g",
                                    static_cast<double>(value));
  if ((written <= 0) || (written >= static_cast<int>(sizeof(numberBuffer)))) {
    m_failed = true;
    return false;
  }

  return append_bytes(numberBuffer, static_cast<std::size_t>(written));
}

bool JsonWriter::append_uint(std::uint32_t value) noexcept {
  char numberBuffer[16] = {};
  const int written =
      std::snprintf(numberBuffer, sizeof(numberBuffer), "%u", value);
  if ((written <= 0) || (written >= static_cast<int>(sizeof(numberBuffer)))) {
    m_failed = true;
    return false;
  }

  return append_bytes(numberBuffer, static_cast<std::size_t>(written));
}

bool JsonWriter::append_uint64(std::uint64_t value) noexcept {
  char numberBuffer[24] = {};
  const int written = std::snprintf(numberBuffer, sizeof(numberBuffer), "%llu",
                                    static_cast<unsigned long long>(value));
  if ((written <= 0) || (written >= static_cast<int>(sizeof(numberBuffer)))) {
    m_failed = true;
    return false;
  }

  return append_bytes(numberBuffer, static_cast<std::size_t>(written));
}

bool JsonWriter::push_container(ContainerKind kind) noexcept {
  if (m_depth >= m_stack.size()) {
    m_failed = true;
    return false;
  }

  ContainerState &state = m_stack[m_depth];
  state.kind = kind;
  state.firstElement = true;
  state.expectingValue = false;
  ++m_depth;
  return true;
}

bool JsonWriter::pop_container(ContainerKind kind) noexcept {
  if (m_depth == 0U) {
    m_failed = true;
    return false;
  }

  const ContainerState &state = m_stack[m_depth - 1U];
  if (state.kind != kind) {
    m_failed = true;
    return false;
  }

  if ((kind == ContainerKind::Object) && state.expectingValue) {
    m_failed = true;
    return false;
  }

  --m_depth;
  return true;
}

void JsonWriter::begin_object() noexcept {
  if (!begin_value()) {
    return;
  }

  if (!append_char('{')) {
    return;
  }

  static_cast<void>(push_container(ContainerKind::Object));
}

void JsonWriter::end_object() noexcept {
  if (!pop_container(ContainerKind::Object)) {
    return;
  }

  static_cast<void>(append_char('}'));
}

void JsonWriter::begin_array(const char *key) noexcept {
  write_key(key);
  begin_array();
}

void JsonWriter::begin_array() noexcept {
  if (!begin_value()) {
    return;
  }

  if (!append_char('[')) {
    return;
  }

  static_cast<void>(push_container(ContainerKind::Array));
}

void JsonWriter::end_array() noexcept {
  if (!pop_container(ContainerKind::Array)) {
    return;
  }

  static_cast<void>(append_char(']'));
}

void JsonWriter::write_key(const char *key) noexcept {
  if (m_failed || (key == nullptr) || (m_depth == 0U)) {
    m_failed = true;
    return;
  }

  ContainerState &state = m_stack[m_depth - 1U];
  if ((state.kind != ContainerKind::Object) || state.expectingValue) {
    m_failed = true;
    return;
  }

  if (!state.firstElement) {
    if (!append_char(',')) {
      return;
    }
  }

  state.firstElement = false;

  if (!append_escaped(key)) {
    return;
  }

  if (!append_char(':')) {
    return;
  }

  state.expectingValue = true;
}

void JsonWriter::write_float(const char *key, float value) noexcept {
  write_key(key);
  write_float_value(value);
}

void JsonWriter::write_uint(const char *key, std::uint32_t value) noexcept {
  write_key(key);
  write_uint_value(value);
}

void JsonWriter::write_uint64(const char *key, std::uint64_t value) noexcept {
  write_key(key);
  write_uint64_value(value);
}

void JsonWriter::write_bool(const char *key, bool value) noexcept {
  write_key(key);
  write_bool_value(value);
}

void JsonWriter::write_string(const char *key, const char *value) noexcept {
  write_key(key);
  write_string_value(value);
}

void JsonWriter::write_float_value(float value) noexcept {
  if (!begin_value()) {
    return;
  }

  static_cast<void>(append_float(value));
}

void JsonWriter::write_uint_value(std::uint32_t value) noexcept {
  if (!begin_value()) {
    return;
  }

  static_cast<void>(append_uint(value));
}

void JsonWriter::write_uint64_value(std::uint64_t value) noexcept {
  if (!begin_value()) {
    return;
  }

  static_cast<void>(append_uint64(value));
}

void JsonWriter::write_bool_value(bool value) noexcept {
  if (!begin_value()) {
    return;
  }

  static_cast<void>(append_cstr(value ? "true" : "false"));
}

void JsonWriter::write_string_value(const char *value) noexcept {
  if (!begin_value()) {
    return;
  }

  static_cast<void>(append_escaped(value));
}

bool JsonWriter::failed() const noexcept { return m_failed; }

bool JsonWriter::ok() const noexcept { return !m_failed && (m_depth == 0U); }

const char *JsonWriter::result() const noexcept {
  return (m_buffer != nullptr) ? m_buffer.get() : "";
}

std::size_t JsonWriter::result_size() const noexcept { return m_pos; }

bool JsonParser::parse(const char *input, std::size_t length) noexcept {
  m_input = input;
  m_length = length;
  m_hasRoot = false;
  m_root = JsonValue{};
  m_scratchCursor = 0U;

  if ((input == nullptr) || (length == 0U)) {
    return false;
  }

  const char *cursor = input;
  const char *end = input + length;
  skip_whitespace(cursor, end);

  JsonValue parsedRoot{};
  if (!parse_value(cursor, end, &parsedRoot)) {
    return false;
  }

  skip_whitespace(cursor, end);
  if (cursor != end) {
    return false;
  }

  m_root = parsedRoot;
  m_hasRoot = true;
  return true;
}

const JsonValue *JsonParser::root() const noexcept {
  return m_hasRoot ? &m_root : nullptr;
}

const JsonValue *
JsonParser::push_scratch(const JsonValue &value) const noexcept {
  if (m_scratchCursor >= m_scratch.size()) {
    return nullptr;
  }

  JsonValue &slot = m_scratch[m_scratchCursor];
  ++m_scratchCursor;
  slot = value;
  return &slot;
}

const JsonValue *
JsonParser::get_object_field(const JsonValue &object,
                             const char *fieldName) const noexcept {
  JsonValue value{};
  if (!get_object_field(object, fieldName, &value)) {
    return nullptr;
  }

  return push_scratch(value);
}

bool JsonParser::get_object_field(const JsonValue &object,
                                  const char *fieldName,
                                  JsonValue *outValue) const noexcept {
  if ((outValue == nullptr) || (fieldName == nullptr) ||
      (object.type != JsonValue::Type::Object) || (object.begin == nullptr) ||
      (object.end == nullptr) || ((object.end - object.begin) < 2)) {
    return false;
  }

  const char *cursor = object.begin + 1;
  const char *end = object.end - 1;
  skip_whitespace(cursor, end);
  if (cursor >= end) {
    return false;
  }

  while (cursor < end) {
    const char *keyBegin = nullptr;
    const char *keyEnd = nullptr;
    if (!parse_string_token(cursor, end, &keyBegin, &keyEnd)) {
      return false;
    }

    skip_whitespace(cursor, end);
    if ((cursor >= end) || (*cursor != ':')) {
      return false;
    }
    ++cursor;

    JsonValue value{};
    if (!parse_value(cursor, end, &value)) {
      return false;
    }

    if (token_equals(keyBegin, keyEnd, fieldName)) {
      *outValue = value;
      return true;
    }

    skip_whitespace(cursor, end);
    if (cursor >= end) {
      break;
    }

    if (*cursor == ',') {
      ++cursor;
      skip_whitespace(cursor, end);
      continue;
    }

    return false;
  }

  return false;
}

const JsonValue *
JsonParser::get_array_element(const JsonValue &array,
                              std::size_t index) const noexcept {
  JsonValue value{};
  if (!get_array_element(array, index, &value)) {
    return nullptr;
  }

  return push_scratch(value);
}

bool JsonParser::get_array_element(const JsonValue &array, std::size_t index,
                                   JsonValue *outValue) const noexcept {
  if ((outValue == nullptr) || (array.type != JsonValue::Type::Array) ||
      (array.begin == nullptr) || (array.end == nullptr) ||
      ((array.end - array.begin) < 2)) {
    return false;
  }

  const char *cursor = array.begin + 1;
  const char *end = array.end - 1;
  skip_whitespace(cursor, end);
  if (cursor >= end) {
    return false;
  }

  std::size_t currentIndex = 0U;
  while (cursor < end) {
    JsonValue value{};
    if (!parse_value(cursor, end, &value)) {
      return false;
    }

    if (currentIndex == index) {
      *outValue = value;
      return true;
    }

    ++currentIndex;

    skip_whitespace(cursor, end);
    if (cursor >= end) {
      break;
    }

    if (*cursor == ',') {
      ++cursor;
      skip_whitespace(cursor, end);
      continue;
    }

    return false;
  }

  return false;
}

std::size_t JsonParser::array_size(const JsonValue &array) const noexcept {
  if ((array.type != JsonValue::Type::Array) || (array.begin == nullptr) ||
      (array.end == nullptr) || ((array.end - array.begin) < 2)) {
    return 0U;
  }

  const char *cursor = array.begin + 1;
  const char *end = array.end - 1;
  skip_whitespace(cursor, end);
  if (cursor >= end) {
    return 0U;
  }

  std::size_t count = 0U;
  while (cursor < end) {
    JsonValue ignored{};
    if (!parse_value(cursor, end, &ignored)) {
      return 0U;
    }
    ++count;

    skip_whitespace(cursor, end);
    if (cursor >= end) {
      break;
    }

    if (*cursor == ',') {
      ++cursor;
      skip_whitespace(cursor, end);
      continue;
    }

    return 0U;
  }

  return count;
}

bool JsonParser::as_float(const JsonValue &value,
                          float *outValue) const noexcept {
  if ((outValue == nullptr) || (value.type != JsonValue::Type::Number) ||
      (value.begin == nullptr) || (value.end == nullptr) ||
      (value.end <= value.begin)) {
    return false;
  }

  const std::size_t length = static_cast<std::size_t>(value.end - value.begin);
  if (length >= 64U) {
    return false;
  }

  char buffer[64] = {};
  std::memcpy(buffer, value.begin, length);
  buffer[length] = '\0';

  char *parseEnd = nullptr;
  const float parsed = std::strtof(buffer, &parseEnd);
  if (parseEnd != (buffer + static_cast<std::ptrdiff_t>(length)) ||
      !std::isfinite(parsed)) {
    return false;
  }

  *outValue = parsed;
  return true;
}

bool JsonParser::as_uint(const JsonValue &value,
                         std::uint32_t *outValue) const noexcept {
  if ((outValue == nullptr) || (value.type != JsonValue::Type::Number) ||
      (value.begin == nullptr) || (value.end == nullptr) ||
      (value.end <= value.begin)) {
    return false;
  }

  const char *cursor = value.begin;
  if (*cursor == '-') {
    return false;
  }

  std::uint64_t parsed = 0U;
  while (cursor < value.end) {
    if (!is_digit(*cursor)) {
      return false;
    }

    parsed = (parsed * 10U) + static_cast<std::uint64_t>(*cursor - '0');
    if (parsed > static_cast<std::uint64_t>(UINT32_MAX)) {
      return false;
    }
    ++cursor;
  }

  *outValue = static_cast<std::uint32_t>(parsed);
  return true;
}

bool JsonParser::as_uint64(const JsonValue &value,
                           std::uint64_t *outValue) const noexcept {
  if ((outValue == nullptr) || (value.type != JsonValue::Type::Number) ||
      (value.begin == nullptr) || (value.end == nullptr) ||
      (value.end <= value.begin)) {
    return false;
  }

  const char *cursor = value.begin;
  if (*cursor == '-') {
    return false;
  }

  std::uint64_t parsed = 0U;
  constexpr std::uint64_t kOverflowGuard = UINT64_MAX / 10U;
  while (cursor < value.end) {
    if (!is_digit(*cursor)) {
      return false;
    }

    const std::uint64_t digit = static_cast<std::uint64_t>(*cursor - '0');
    if ((parsed > kOverflowGuard) ||
        ((parsed == kOverflowGuard) && (digit > UINT64_MAX % 10U))) {
      return false;
    }

    parsed = (parsed * 10U) + digit;
    ++cursor;
  }

  *outValue = parsed;
  return true;
}

bool JsonParser::as_bool(const JsonValue &value,
                         bool *outValue) const noexcept {
  if ((outValue == nullptr) || (value.type != JsonValue::Type::Bool) ||
      (value.begin == nullptr) || (value.end == nullptr) ||
      (value.end <= value.begin)) {
    return false;
  }

  if (token_equals(value.begin, value.end, "true")) {
    *outValue = true;
    return true;
  }

  if (token_equals(value.begin, value.end, "false")) {
    *outValue = false;
    return true;
  }

  return false;
}

bool JsonParser::as_string(const JsonValue &value, const char **outBegin,
                           std::size_t *outLength) const noexcept {
  if ((outBegin == nullptr) || (outLength == nullptr) ||
      (value.type != JsonValue::Type::String) || (value.begin == nullptr) ||
      (value.end == nullptr) || (value.end < value.begin)) {
    return false;
  }

  *outBegin = value.begin;
  *outLength = static_cast<std::size_t>(value.end - value.begin);
  return true;
}

} // namespace engine::core
