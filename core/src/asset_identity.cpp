// Implements the pure operations over the asset identity value types: the
// table-keying hash and the reporting order. Generation and the text
// forms live with content, which owns what a GUID means.

#include "engine/core/asset_identity.h"

#include "engine/core/hash.h"

namespace engine::core {

std::uint64_t asset_guid_hash(const AssetGuid &guid) noexcept {
  std::uint64_t hash = kFnv1a64Offset;
  for (std::size_t i = 0U; i < 8U; ++i) {
    const std::size_t shift = (7U - i) * 8U;
    hash = fnv1a_64_append(
        hash, static_cast<std::uint8_t>((guid.high >> shift) & 0xFFULL));
  }
  for (std::size_t i = 0U; i < 8U; ++i) {
    const std::size_t shift = (7U - i) * 8U;
    hash = fnv1a_64_append(
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

} // namespace engine::core
