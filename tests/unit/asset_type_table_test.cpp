// Pins the asset type table's contract: every row has a label, descriptors
// answer for every tag, and the path classifier maps each row's source and
// cooked suffixes back to that row regardless of case, preferring the
// longest suffix and answering Unknown for anything else.

#include <cstdio>
#include <cstring>

#include "../test_harness.h"
#include "engine/content/asset_type_table.h"

namespace {

namespace ct = engine::content;

/// Builds "probe<suffix>" with the suffix upper-cased in place.
void upper_probe(const char *suffix, char *out, std::size_t size) noexcept {
  std::snprintf(out, size, "probe%s", suffix);
  for (char *c = out + 5; *c != '\0'; ++c) {
    if ((*c >= 'a') && (*c <= 'z')) {
      *c = static_cast<char>(*c - 'a' + 'A');
    }
  }
}

} // namespace

/// Runs this executable or test program.
int main() {
  engine::tests::TestContext ctx;

  for (std::size_t i = 0U; i < ct::kAssetTypeCount; ++i) {
    const ct::AssetTypeTag tag = static_cast<ct::AssetTypeTag>(i);
    const ct::AssetTypeDescriptor &row = ct::asset_type_descriptor(tag);
    ctx.check(row.tag == tag, "descriptor answers for its own tag");
    ctx.check((row.label != nullptr) && (row.label[0] != '\0'),
              "every row has a label");
    ctx.check(std::strcmp(ct::asset_type_label(tag), row.label) == 0,
              "the label accessor reads the row");
    for (std::size_t s = 0U; s < row.sourceSuffixCount; ++s) {
      char probe[64] = {};
      upper_probe(row.sourceSuffixes[s], probe, sizeof(probe));
      const ct::AssetClassification c = ct::classify_asset_path(probe);
      ctx.check((c.tag == tag) && c.source,
                "a source suffix classifies as its row, as source");
    }
    for (std::size_t s = 0U; s < row.cookedSuffixCount; ++s) {
      char probe[64] = {};
      upper_probe(row.cookedSuffixes[s], probe, sizeof(probe));
      const ct::AssetClassification c = ct::classify_asset_path(probe);
      ctx.check((c.tag == tag) && !c.source,
                "a cooked suffix classifies as its row, not source");
    }
    if (row.policy == ct::AssetSourcePolicy::Derived) {
      ctx.check(row.sourceSuffixCount == 0U,
                "a derived type has no authored source form");
    }
  }

  ctx.check(ct::asset_type_descriptor(static_cast<ct::AssetTypeTag>(200)).tag ==
                ct::AssetTypeTag::Unknown,
            "a value outside the table reads as Unknown");
  ctx.check(ct::classify_asset_path(nullptr).tag == ct::AssetTypeTag::Unknown,
            "a null path is Unknown");
  ctx.check(ct::classify_asset_path("").tag == ct::AssetTypeTag::Unknown,
            "an empty path is Unknown");
  ctx.check(ct::classify_asset_path("noext").tag == ct::AssetTypeTag::Unknown,
            "a path without a suffix is Unknown");
  // The three kinds that used to need their bytes read to be told apart:
  // a suffix names the kind, so the name alone decides.
  ctx.check(ct::classify_asset_path("levels/hub.scene").tag ==
                ct::AssetTypeTag::Scene,
            "a scene is classified by its .scene suffix");
  ctx.check(ct::classify_asset_path("props/crate.prefab").tag ==
                ct::AssetTypeTag::Prefab,
            "a prefab is classified by its .prefab suffix");
  ctx.check(ct::classify_asset_path("mats/brass.mat").tag ==
                ct::AssetTypeTag::Material,
            "a material is classified by its .mat suffix");
  ctx.check(ct::classify_asset_path("hero.animctrl").tag ==
                ct::AssetTypeTag::AnimationController,
            "a controller is classified by its .animctrl suffix");
  ctx.check(ct::classify_asset_path("scene.json").tag ==
                ct::AssetTypeTag::Unknown,
            "the serialization format never names a kind: .json is Unknown");
  ctx.check(ct::classify_asset_path("hub.scene.json").tag ==
                ct::AssetTypeTag::Unknown,
            "a kind suffix only counts at the end of the path");
  ctx.check(ct::classify_asset_path("props/coin.mesh.meta").tag ==
                ct::AssetTypeTag::Unknown,
            "a sidecar is not the asset it sits beside");
  ctx.check(ct::classify_asset_path("anim/walk.animat").tag ==
                ct::AssetTypeTag::Unknown,
            "a suffix matches on its dot, not on trailing letters");
  ctx.check(ct::classify_asset_path(".mesh").tag == ct::AssetTypeTag::Mesh,
            "a path that is only the suffix still matches");
  ctx.check(ct::classify_asset_path("mesh").tag == ct::AssetTypeTag::Unknown,
            "a suffix needs its dot");

  return ctx.finish("asset_type_table");
}
