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
//
// The GUID and reference value types themselves are core's, so that a
// component defined below this module can carry one; this header is
// where they acquire generation, text and derivation.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/asset_identity.h"

namespace engine::content {

// --- AssetGuid ---------------------------------------------------------

// The same types as core's, not copies: a caller that names them through
// either namespace holds the one definition.
using core::AssetGuid;
using core::AssetRef;
using core::asset_guid_hash;
using core::asset_guid_is_valid;
using core::asset_guid_precedes;
using core::asset_ref_is_valid;
using core::asset_ref_primary;
using core::kNilAssetGuid;

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

/// The GUID a built-in asset has. Built-ins — the primitive meshes under
/// "builtin://" — ship with the engine, carry no sidecar, and must name
/// the same asset on every machine and in every build, so their GUID is
/// derived from the canonical spelling of their virtual path rather than
/// generated: FNV-1a over the path into each half, with the version
/// nibble set to 8 (RFC 9562's custom version; the derivation is not the
/// SHA-1 that version 5 would promise) and the RFC variant bits, so it can
/// never coincide with a generated v4 value. Returns nil for a null,
/// empty or uncanonicalizable path.
///
/// A project asset never gets its GUID this way: that would tie identity
/// back to location, which is the thing a GUID exists to sever.
AssetGuid builtin_asset_guid(const char *virtualPath) noexcept;

/// The stable local id of a sub-asset named `subName` — the part of a
/// cooked output's name after its source's stem, such as "walk.anim" for
/// "character.walk.anim" produced from "character.gltf". Returns 0 for a
/// null or empty name, which is the primary asset's id.
std::uint64_t asset_local_id(const char *subName) noexcept;

/// Parses a local id as the cook stamp and the reference text carry it:
/// exactly 16 lowercase hex digits at `text`. False for any other length
/// or any other character, uppercase included — the writers only ever
/// emit this one shape, so anything else is an edit to refuse.
bool parse_asset_local_id(const char *text, std::size_t length,
                          std::uint64_t *out) noexcept;

/// Characters in the longer reference text form, terminator excluded:
/// the GUID, '#', and the 16-digit local id.
inline constexpr std::size_t kAssetRefTextLength =
    kAssetGuidTextLength + 1U + 16U;

/// Writes the reference's text form plus a terminator into `out`: the bare
/// GUID for a primary asset, "<guid>#<local id>" for a sub-asset. Two
/// shapes so a document that names a whole file stays readable at a glance
/// while one that names a clip of many still can. False with `out`
/// emptied when `capacity` is under what the shape needs plus one.
bool format_asset_ref(const AssetRef &ref, char *out,
                      std::size_t capacity) noexcept;

/// Parses either text form back; false with `*out` nil for any other
/// shape — a wrong length, a separator other than '#', a local id that is
/// not exactly 16 lowercase hex digits, or the sub-asset shape carrying
/// local id 0, which names the primary and so contradicts itself. As
/// strict as parse_asset_guid, for the same reason.
bool parse_asset_ref(const char *text, AssetRef *out) noexcept;

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
