// Declares the JSON form of an asset reference: the one codec every
// document that names an asset by identity reads and writes, so a scene,
// a prefab and a material spell a reference the same way.

#pragma once

#include "engine/core/asset_identity.h"

namespace engine::core {
class JsonParser;
class JsonWriter;
struct JsonValue;
} // namespace engine::core

namespace engine::content {

/// Writes `ref` under `key` in its canonical text form. Writes nothing
/// when the reference is nil, so a field naming no asset stays absent
/// from the document rather than carrying a nil identity string.
void write_asset_ref(core::JsonWriter &writer, const char *key,
                     const core::AssetRef &ref) noexcept;

/// Reads an asset reference from the string at `value`. False for any
/// non-string, and for any text the reference parser refuses: an
/// identity that does not parse is an authored field to reject, never one
/// to silently default, because a defaulted identity names a different
/// asset than the author wrote.
bool read_asset_ref(const core::JsonParser &parser,
                    const core::JsonValue &value,
                    core::AssetRef *outRef) noexcept;

} // namespace engine::content
