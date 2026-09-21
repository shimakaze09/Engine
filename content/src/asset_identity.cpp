// Implements the three asset identities: v4 GUID generation and its
// canonical text round trip, the canonical-path key, and the content
// hash. Each identity's derivation lives here so no call site can invent
// its own and drift.

#include "engine/content/asset_identity.h"

#include <cctype>
#include <cstring>

#include "engine/core/hash.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/core/vfs.h"

namespace engine::content {
namespace {

/// Lowercase hex digits, in the order format_asset_guid emits them.
constexpr char kHexDigits[] = "0123456789abcdef";

/// Value of one hex digit, or 16 when `ch` is not one.
unsigned int hex_value(char ch) noexcept {
  if ((ch >= '0') && (ch <= '9')) {
    return static_cast<unsigned int>(ch - '0');
  }
  if ((ch >= 'a') && (ch <= 'f')) {
    return static_cast<unsigned int>(ch - 'a') + 10U;
  }
  if ((ch >= 'A') && (ch <= 'F')) {
    return static_cast<unsigned int>(ch - 'A') + 10U;
  }
  return 16U;
}

/// Writes `count` hex digits of `value`'s low nibbles, most significant
/// first, and returns the position after them.
std::size_t write_hex(char *out, std::size_t offset, std::uint64_t value,
                      std::size_t count) noexcept {
  for (std::size_t i = 0U; i < count; ++i) {
    const std::size_t shift = (count - 1U - i) * 4U;
    out[offset + i] = kHexDigits[(value >> shift) & 0xFULL];
  }
  return offset + count;
}

/// The hyphen positions of the canonical form. Every other position holds
/// a hex digit.
constexpr bool is_hyphen_position(std::size_t index) noexcept {
  return (index == 8U) || (index == 13U) || (index == 18U) || (index == 23U);
}

/// ASCII lowercase, for the case-collision comparison only. Deliberately
/// not applied to a PathKey: identity must not depend on case folding.
char lower_ascii_char(char ch) noexcept {
  if ((ch >= 'A') && (ch <= 'Z')) {
    return static_cast<char>(ch - 'A' + 'a');
  }
  return ch;
}

} // namespace

AssetGuid generate_asset_guid() noexcept {
  unsigned char bytes[16] = {};
  if (!core::platform_random_bytes(bytes, sizeof(bytes))) {
    core::log_message(core::LogLevel::Error, "assets",
                      "asset identity: the platform refused entropy, so no "
                      "GUID was generated; the transaction must fail rather "
                      "than store a nil identity");
    return kNilAssetGuid;
  }

  // UUID v4: version 4 in the high nibble of byte 6, variant 10xx in the
  // top bits of byte 8. Everything else stays random.
  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0FU) | 0x40U);
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3FU) | 0x80U);

  AssetGuid guid{};
  for (std::size_t i = 0U; i < 8U; ++i) {
    guid.high = (guid.high << 8U) | static_cast<std::uint64_t>(bytes[i]);
    guid.low = (guid.low << 8U) | static_cast<std::uint64_t>(bytes[i + 8U]);
  }
  return guid;
}

bool format_asset_guid(const AssetGuid &guid, char *out,
                       std::size_t capacity) noexcept {
  if ((out == nullptr) || (capacity == 0U)) {
    return false;
  }
  out[0] = '\0';
  if (capacity < (kAssetGuidTextLength + 1U)) {
    return false;
  }

  // 8-4-4-4-12: the first three groups and the first half of the fourth
  // come out of `high`, the rest out of `low`.
  std::size_t offset = write_hex(out, 0U, guid.high >> 32U, 8U);
  out[offset] = '-';
  ++offset;
  offset = write_hex(out, offset, guid.high >> 16U, 4U);
  out[offset] = '-';
  ++offset;
  offset = write_hex(out, offset, guid.high, 4U);
  out[offset] = '-';
  ++offset;
  offset = write_hex(out, offset, guid.low >> 48U, 4U);
  out[offset] = '-';
  ++offset;
  offset = write_hex(out, offset, guid.low, 12U);
  out[offset] = '\0';
  return true;
}

bool parse_asset_guid(const char *text, AssetGuid *out) noexcept {
  if (out == nullptr) {
    return false;
  }
  *out = kNilAssetGuid;
  if (text == nullptr) {
    return false;
  }
  if (std::strlen(text) != kAssetGuidTextLength) {
    return false;
  }

  std::uint64_t high = 0U;
  std::uint64_t low = 0U;
  std::size_t digits = 0U;
  for (std::size_t i = 0U; i < kAssetGuidTextLength; ++i) {
    if (is_hyphen_position(i)) {
      if (text[i] != '-') {
        return false;
      }
      continue;
    }
    const unsigned int value = hex_value(text[i]);
    if (value > 15U) {
      return false;
    }
    // The first sixteen digits are the high half, the last sixteen the low.
    if (digits < 16U) {
      high = (high << 4U) | static_cast<std::uint64_t>(value);
    } else {
      low = (low << 4U) | static_cast<std::uint64_t>(value);
    }
    ++digits;
  }
  if (digits != 32U) {
    return false;
  }

  out->high = high;
  out->low = low;
  return true;
}

std::uint64_t asset_guid_hash(const AssetGuid &guid) noexcept {
  std::uint64_t hash = core::kFnv1a64Offset;
  for (std::size_t i = 0U; i < 8U; ++i) {
    const std::size_t shift = (7U - i) * 8U;
    hash = core::fnv1a_64_append(
        hash, static_cast<std::uint8_t>((guid.high >> shift) & 0xFFULL));
  }
  for (std::size_t i = 0U; i < 8U; ++i) {
    const std::size_t shift = (7U - i) * 8U;
    hash = core::fnv1a_64_append(
        hash, static_cast<std::uint8_t>((guid.low >> shift) & 0xFFULL));
  }
  return hash;
}

bool asset_guid_precedes(const AssetGuid &a, const AssetGuid &b) noexcept {
  if (a.high != b.high) {
    return a.high < b.high;
  }
  return a.low < b.low;
}

std::uint64_t asset_local_id(const char *subName) noexcept {
  if ((subName == nullptr) || (subName[0] == '\0')) {
    return 0U;
  }
  std::uint64_t hash = core::kFnv1a64Offset;
  for (const unsigned char *cursor =
           reinterpret_cast<const unsigned char *>(subName);
       *cursor != 0U; ++cursor) {
    hash = core::fnv1a_64_append(hash, static_cast<std::uint8_t>(*cursor));
  }
  // Zero is the primary asset's reserved id, so the one name that would
  // land on it is nudged rather than being made to mean "the main asset".
  if (hash == 0U) {
    hash = 1ULL;
  }
  return hash;
}

PathKey make_path_key(const char *virtualPath) noexcept {
  char canonical[core::kMaxVirtualPathLength] = {};
  if (!core::canonical_virtual_path(virtualPath, canonical,
                                    sizeof(canonical))) {
    return kInvalidPathKey;
  }

  std::uint64_t hash = core::kFnv1a64Offset;
  for (const unsigned char *cursor =
           reinterpret_cast<const unsigned char *>(canonical);
       *cursor != 0U; ++cursor) {
    hash = core::fnv1a_64_append(hash, static_cast<std::uint8_t>(*cursor));
  }
  // Zero is the reserved "no key" value, so the one input that would
  // produce it is nudged instead.
  if (hash == kInvalidPathKey.value) {
    hash = 1ULL;
  }
  return PathKey{hash};
}

bool path_keys_collide_by_case(const char *virtualPathA,
                               const char *virtualPathB) noexcept {
  char a[core::kMaxVirtualPathLength] = {};
  char b[core::kMaxVirtualPathLength] = {};
  if (!core::canonical_virtual_path(virtualPathA, a, sizeof(a)) ||
      !core::canonical_virtual_path(virtualPathB, b, sizeof(b))) {
    return false;
  }
  if (std::strcmp(a, b) == 0) {
    return false; // the same path, not a conflict
  }

  for (std::size_t i = 0U;; ++i) {
    const char left = lower_ascii_char(a[i]);
    const char right = lower_ascii_char(b[i]);
    if (left != right) {
      return false;
    }
    if (left == '\0') {
      return true;
    }
  }
}

ContentHash make_content_hash(const void *bytes, std::size_t size) noexcept {
  if (bytes == nullptr) {
    return kInvalidContentHash;
  }
  const auto *cursor = static_cast<const unsigned char *>(bytes);
  std::uint64_t hash = core::kFnv1a64Offset;
  for (std::size_t i = 0U; i < size; ++i) {
    hash = core::fnv1a_64_append(hash, static_cast<std::uint8_t>(cursor[i]));
  }
  if (hash == kInvalidContentHash.value) {
    hash = 1ULL;
  }
  return ContentHash{hash};
}

} // namespace engine::content
