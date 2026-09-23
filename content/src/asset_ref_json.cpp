// Implements the JSON form of an asset reference on top of the reference
// text codec, strict on read and silent for a nil reference on write.

#include "engine/content/asset_ref_json.h"

#include "engine/content/asset_identity.h"
#include "engine/core/json.h"

namespace engine::content {

void write_asset_ref(core::JsonWriter &writer, const char *key,
                     const core::AssetRef &ref) noexcept {
  if ((key == nullptr) || !core::asset_ref_is_valid(ref)) {
    return;
  }
  char text[kAssetRefTextLength + 1U] = {};
  // The buffer is the exact size the longest form needs, so the formatter
  // has no failing case left here: it refuses only a null or too-small
  // destination. The check stays as a guard against that buffer shrinking,
  // and never as a path that drops an authored identity in silence.
  if (!format_asset_ref(ref, text, sizeof(text))) {
    return;
  }
  writer.write_string(key, text);
}

bool read_asset_ref(const core::JsonParser &parser,
                    const core::JsonValue &value,
                    core::AssetRef *outRef) noexcept {
  if (outRef == nullptr) {
    return false;
  }
  *outRef = core::AssetRef{};
  char text[kAssetRefTextLength + 1U] = {};
  if (!parser.copy_string_strict(value, text, sizeof(text))) {
    return false;
  }
  return parse_asset_ref(text, outRef);
}

} // namespace engine::content
