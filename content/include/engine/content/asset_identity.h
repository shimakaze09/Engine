// Declares the three asset identities the content layer keeps apart, and
// the operations over each. They exist as distinct types precisely so the
// compiler refuses to mix them: one asset's persistent identity, its
// current location, and the bytes it holds answer different questions and
// change at different times.
//
//   AssetGuid   persistent logical identity. Survives rename, move, a
//               case-only rename and any content edit. Generated once, in
//               an explicit import, migration or Duplicate transaction,
//               and never on a read, load or cook path.
//   PathKey     canonical-virtual-path key. Transitional: it is how a
//               reference written before GUIDs is still resolved, and how
//               a catalog answers "what is at this path". Changes whenever
//               the asset moves.
//   ContentHash the bytes. Drives cook and cache invalidation, and nothing
//               else. Changes on every content edit.
//
// None of the three converts to another, implicitly or otherwise. A
// function that needs two of them takes two parameters.

#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::content {

// --- AssetGuid ---------------------------------------------------------

/// 128-bit persistent logical asset identity, held as two 64-bit halves
/// in big-endian reading order: `high` is the first eight bytes of the
/// canonical text form, `low` the last eight.
struct AssetGuid final {
  std::uint64_t high = 0U;
  std::uint64_t low = 0U;

  friend constexpr bool operator==(const AssetGuid &,
                                   const AssetGuid &) = default;
};

/// The all-zero GUID, which no asset owns. A nil UUID is not a valid v4
/// value, so it can never collide with a generated one.
inline constexpr AssetGuid kNilAssetGuid{};

/// True for any GUID an asset may own.
constexpr bool asset_guid_is_valid(const AssetGuid &guid) noexcept {
  return !((guid.high == 0U) && (guid.low == 0U));
}

/// Characters in the canonical text form, terminator excluded:
/// "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx".
inline constexpr std::size_t kAssetGuidTextLength = 36U;

/// Generates a fresh random (UUID v4) GUID from OS entropy, with the
/// version and variant bits set. Returns kNilAssetGuid when the platform
/// refuses entropy, which the caller must treat as a failed transaction
/// rather than proceeding with a nil identity.
///
/// Call this ONLY from an explicit import, migration or Duplicate
/// transaction. A read, load or cook path that generates an identity has
/// invented one behind the author's back, and two machines running the
/// same cook would disagree.
AssetGuid generate_asset_guid() noexcept;

/// Writes the canonical lowercase text form plus a terminator into `out`;
/// false with `out` emptied when `capacity` is under
/// kAssetGuidTextLength + 1. This is the only form written to authored
/// metadata, so a sidecar stays diffable and merge-resolvable.
bool format_asset_guid(const AssetGuid &guid, char *out,
                       std::size_t capacity) noexcept;

/// Parses the canonical text form, accepting either letter case; false
/// with `*out` left nil for any other shape, including a wrong length, a
/// misplaced hyphen, a non-hex digit or trailing characters. Strict on
/// purpose: a GUID that parses loosely is a GUID that can be silently
/// corrupted by an editing mistake.
bool parse_asset_guid(const char *text, AssetGuid *out) noexcept;

/// A 64-bit hash of the GUID, for keying a fixed-capacity table. Not an
/// identity: two different GUIDs may hash alike, so a table must still
/// compare the full value.
std::uint64_t asset_guid_hash(const AssetGuid &guid) noexcept;

/// Deterministic total order over GUIDs, for reporting a set of them in a
/// stable sequence. Never use it to choose between two assets that claim
/// one GUID: that collision is an error to report, not to resolve.
bool asset_guid_precedes(const AssetGuid &a, const AssetGuid &b) noexcept;

/// Names one asset among the several a single source file can produce.
/// One glTF here yields a mesh, a skeleton and several clips, and its
/// import settings select a mesh and primitive inside it, so a GUID
/// naming the file cannot on its own name "the walk clip of character".
/// `guid` identifies the source, `localId` the asset within it — the same
/// split Unity makes with its {guid, fileID} pairs.
///
/// localId 0 is the source's primary asset, which is the only one most
/// sources have. Sub-assets take asset_local_id, so the value is derived
/// from the name rather than assigned from a counter: it stays the same
/// across recooks, across machines, and across a rebuild from scratch.
struct AssetRef final {
  AssetGuid guid{};
  std::uint64_t localId = 0U;

  friend constexpr bool operator==(const AssetRef &, const AssetRef &) = default;
};

/// The primary asset of `guid`.
constexpr AssetRef asset_ref_primary(const AssetGuid &guid) noexcept {
  return AssetRef{guid, 0U};
}

/// True when the reference names an asset at all.
constexpr bool asset_ref_is_valid(const AssetRef &ref) noexcept {
  return asset_guid_is_valid(ref.guid);
}

/// The stable local id of a sub-asset named `subName` — the part of a
/// cooked output's name after its source's stem, such as "walk.anim" for
/// "character.walk.anim" produced from "character.gltf". Returns 0 for a
/// null or empty name, which is the primary asset's id.
std::uint64_t asset_local_id(const char *subName) noexcept;

// --- PathKey -----------------------------------------------------------

/// Key derived from an asset's canonical virtual path. Transitional: it
/// answers "what is at this location" and resolves references written
/// before GUIDs existed, so it changes when the asset moves.
struct PathKey final {
  std::uint64_t value = 0U;

  friend constexpr bool operator==(const PathKey &, const PathKey &) = default;
};

/// The key no path owns.
inline constexpr PathKey kInvalidPathKey{};

/// True for any key a path may own.
constexpr bool path_key_is_valid(const PathKey &key) noexcept {
  return key.value != 0U;
}

/// Derives the key from `virtualPath`'s canonical spelling, so every
/// spelling of one location gives one key. Returns kInvalidPathKey for a
/// path that names no asset — empty, nothing but separators or "."
/// segments, carrying a "..", or too long to hold whole.
///
/// Case is preserved on every platform: "assets/Foo.png" and
/// "assets/foo.png" are different keys everywhere. Two project assets
/// whose canonical paths differ only by case are a portability conflict
/// for the catalog to report, never something this function merges.
PathKey make_path_key(const char *virtualPath) noexcept;

/// True when two canonical paths differ only by ASCII letter case — the
/// portability conflict above. False when they are equal, or differ in
/// any way case folding would not explain.
bool path_keys_collide_by_case(const char *virtualPathA,
                               const char *virtualPathB) noexcept;

// --- ContentHash -------------------------------------------------------

/// Hash of an asset's bytes. Drives cook and cache invalidation only: it
/// changes on every content edit, so it can never carry identity.
struct ContentHash final {
  std::uint64_t value = 0U;

  friend constexpr bool operator==(const ContentHash &,
                                   const ContentHash &) = default;
};

/// The hash of nothing, which no content owns.
inline constexpr ContentHash kInvalidContentHash{};

/// True for any hash real content may own.
constexpr bool content_hash_is_valid(const ContentHash &hash) noexcept {
  return hash.value != 0U;
}

/// Hashes `size` bytes at `bytes`. Returns kInvalidContentHash when
/// `bytes` is null; empty content hashes to a valid, fixed value, since
/// an empty file is content that a cook must still invalidate against.
ContentHash make_content_hash(const void *bytes, std::size_t size) noexcept;

} // namespace engine::content
