// Declares the cook contract shared by the asset packer (which writes cook
// stamps) and the runtime (which validates them before a load): the stamp
// text schema and the importer tool version. One definition keeps the
// writer and the reader in agreement, so a bump on either side is a bump
// on both and a cooked tree from another tool is refused instead of
// silently loaded.

#pragma once

#include <cstdint>

namespace engine::content {

/// Layout version of the `.cookstamp` text: which key lines exist and
/// what they mean. Schema 3 carries the TOOL_VERSION, PLATFORM and OUTPUT
/// manifest lines. A stamp declaring a higher schema was written by a
/// newer packer whose lines this reader cannot interpret, so it certifies
/// nothing here; a lower or absent schema is a pre-manifest stamp.
inline constexpr std::uint32_t kCookStampSchema = 3U;

/// Importer contract version baked into every cook stamp: bump whenever
/// the cooked output format or import semantics change, so existing
/// outputs recook once instead of silently keeping stale bytes, and the
/// runtime refuses a stamp from any other version until the tree is
/// recooked. Stamps written before this key existed read as version 0 in
/// the packer and always recook.
inline constexpr std::uint32_t kCookToolVersion = 3U;

} // namespace engine::content
