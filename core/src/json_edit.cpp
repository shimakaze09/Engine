// Implements byte-preserving JSON document edits: replacing or inserting
// one top-level field while leaving every other byte untouched, so tools
// that understand only part of a schema cannot destroy the fields other
// tools rely on (audit H-21).

#include "engine/core/json.h"

#include <cstddef>
#include <cstring>

namespace engine::core {

namespace {

/// Returns the index one past a string's closing quote; `length` on
/// malformed or unterminated input. `i` must point at the opening quote.
std::size_t skip_json_string(const char *text, std::size_t length,
                             std::size_t i) noexcept {
  ++i;
  while (i < length) {
    if (text[i] == '\\') {
      i += 2U;
      continue;
    }
    if (text[i] == '"') {
      return i + 1U;
    }
    ++i;
  }
  return length;
}

/// Returns the first non-whitespace index at or after `i`.
std::size_t skip_json_whitespace(const char *text, std::size_t length,
                                 std::size_t i) noexcept {
  while ((i < length) &&
         ((text[i] == ' ') || (text[i] == '\t') || (text[i] == '\n') ||
          (text[i] == '\r'))) {
    ++i;
  }
  return i;
}

/// Returns the end (exclusive) of the JSON value starting at `i`:
/// string-aware brace/bracket matching for containers, quote matching
/// for strings, and a bare-token scan for numbers/booleans/null.
std::size_t json_value_end(const char *text, std::size_t length,
                           std::size_t i) noexcept {
  if (i >= length) {
    return length;
  }
  const char first = text[i];
  if (first == '"') {
    return skip_json_string(text, length, i);
  }
  if ((first == '{') || (first == '[')) {
    std::size_t depth = 0U;
    while (i < length) {
      const char c = text[i];
      if (c == '"') {
        i = skip_json_string(text, length, i);
        continue;
      }
      if ((c == '{') || (c == '[')) {
        ++depth;
      } else if ((c == '}') || (c == ']')) {
        --depth;
        if (depth == 0U) {
          return i + 1U;
        }
      }
      ++i;
    }
    return length;
  }
  while ((i < length) && (text[i] != ',') && (text[i] != '}') &&
         (text[i] != ']') && (text[i] != ' ') && (text[i] != '\t') &&
         (text[i] != '\n') && (text[i] != '\r')) {
    ++i;
  }
  return i;
}

} // namespace

bool json_replace_top_level_field(const char *documentText,
                                  std::size_t documentLength,
                                  const char *fieldName,
                                  const char *valueText, char *outBuffer,
                                  std::size_t outCapacity,
                                  std::size_t *outLength) noexcept {
  if ((documentText == nullptr) || (fieldName == nullptr) ||
      (valueText == nullptr) || (outBuffer == nullptr) ||
      (outLength == nullptr)) {
    return false;
  }

  const std::size_t nameLength = std::strlen(fieldName);
  const std::size_t rootBegin =
      skip_json_whitespace(documentText, documentLength, 0U);
  if ((rootBegin >= documentLength) || (documentText[rootBegin] != '{')) {
    return false;
  }
  const std::size_t rootEnd =
      json_value_end(documentText, documentLength, rootBegin);
  if ((rootEnd <= rootBegin) ||
      (skip_json_whitespace(documentText, documentLength, rootEnd) !=
       documentLength)) {
    return false;
  }

  // Walk the depth-1 fields to find the target's value byte range.
  std::size_t replaceBegin = 0U;
  std::size_t replaceEnd = 0U;
  bool found = false;
  bool sawAnyField = false;
  std::size_t cursor = rootBegin + 1U;
  while (true) {
    cursor = skip_json_whitespace(documentText, documentLength, cursor);
    if (cursor >= documentLength) {
      return false;
    }
    if (documentText[cursor] == '}') {
      break;
    }
    if (documentText[cursor] == ',') {
      ++cursor;
      continue;
    }
    if (documentText[cursor] != '"') {
      return false;
    }
    const std::size_t keyBegin = cursor + 1U;
    const std::size_t keyClose =
        skip_json_string(documentText, documentLength, cursor);
    if (keyClose >= documentLength) {
      return false;
    }
    const std::size_t keyLength = keyClose - 1U - keyBegin;
    cursor = skip_json_whitespace(documentText, documentLength, keyClose);
    if ((cursor >= documentLength) || (documentText[cursor] != ':')) {
      return false;
    }
    cursor = skip_json_whitespace(documentText, documentLength, cursor + 1U);
    const std::size_t valueEnd =
        json_value_end(documentText, documentLength, cursor);
    sawAnyField = true;
    if (!found && (keyLength == nameLength) &&
        (std::memcmp(documentText + keyBegin, fieldName, nameLength) == 0)) {
      replaceBegin = cursor;
      replaceEnd = valueEnd;
      found = true;
    }
    cursor = valueEnd;
  }

  const std::size_t valueLength = std::strlen(valueText);
  if (found) {
    const std::size_t newLength =
        documentLength - (replaceEnd - replaceBegin) + valueLength;
    if ((newLength + 1U) > outCapacity) {
      return false;
    }
    std::memcpy(outBuffer, documentText, replaceBegin);
    std::memcpy(outBuffer + replaceBegin, valueText, valueLength);
    std::memcpy(outBuffer + replaceBegin + valueLength,
                documentText + replaceEnd, documentLength - replaceEnd);
    outBuffer[newLength] = '\0';
    *outLength = newLength;
    return true;
  }

  // Missing field: insert it just before the root's closing brace.
  const std::size_t closeBrace = rootEnd - 1U;
  const char *separator = sawAnyField ? ",\n  " : "\n  ";
  const std::size_t separatorLength = std::strlen(separator);
  const std::size_t insertionLength =
      separatorLength + 1U + nameLength + 3U + valueLength + 1U;
  const std::size_t newLength = documentLength + insertionLength;
  if ((newLength + 1U) > outCapacity) {
    return false;
  }
  std::size_t out = 0U;
  std::memcpy(outBuffer, documentText, closeBrace);
  out = closeBrace;
  std::memcpy(outBuffer + out, separator, separatorLength);
  out += separatorLength;
  outBuffer[out++] = '"';
  std::memcpy(outBuffer + out, fieldName, nameLength);
  out += nameLength;
  outBuffer[out++] = '"';
  outBuffer[out++] = ':';
  outBuffer[out++] = ' ';
  std::memcpy(outBuffer + out, valueText, valueLength);
  out += valueLength;
  outBuffer[out++] = '\n';
  std::memcpy(outBuffer + out, documentText + closeBrace,
              documentLength - closeBrace);
  out += documentLength - closeBrace;
  outBuffer[out] = '\0';
  *outLength = out;
  return true;
}

} // namespace engine::core
