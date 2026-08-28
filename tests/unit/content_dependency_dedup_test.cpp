// Regression for #337: load_with_dependencies must invoke its callback
// exactly once per distinct asset, however large the graph. The traversal
// used to record loaded ids in a 256-entry array and silently stop
// recording once it was full, so a dependency shared by many assets was
// loaded again for every dependent past that point while the call still
// reported success. Drives the production content::load_with_dependencies
// entry point over graphs that cross that bound, plus the deduplication
// boundaries: a shared dependency, ids the store holds no record for, a
// graph that exhausts the side list for those ids, a leaf with no
// dependencies, a cycle behind many loaded assets, and a repeated
// traversal over the same store.

#include "engine/content/asset_metadata.h"
#include "engine/content/metadata_store.h"

#include <cstddef>
#include <cstdio>
#include <memory>
#include <new>

namespace {

using engine::content::AssetId;
using engine::content::AssetMetadata;
using engine::content::AssetTypeTag;
using engine::content::MetadataStore;

int g_failures = 0;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);         \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

/// Lowest id the cases hand out. Ids are sequential from here so a load
/// record can index its tallies by `id - kFirstId` instead of searching.
constexpr AssetId kFirstId = 1000ULL;

/// Widest tally the cases need: the case that exhausts the traversal's
/// record of ids without metadata reaches 288 of them, on top of the ids
/// the earlier cases hand out.
constexpr std::size_t kMaxTrackedAssets = 1024U;

/// Per-traversal tally of which assets ran their callback and in what
/// order, so a case can assert both exactly-once and dependency-first.
struct LoadRecord final {
  std::size_t counts[kMaxTrackedAssets] = {};
  std::size_t order[kMaxTrackedAssets] = {};
  std::size_t totalLoads = 0U;
  bool outOfRange = false;
};

LoadRecord g_record{};

/// Records one load. Ids outside the tracked range flag the record rather
/// than writing past it, so a mis-built case fails loudly.
bool record_load(AssetId id, void *userData) noexcept {
  static_cast<void>(userData);
  if ((id < kFirstId) || ((id - kFirstId) >= kMaxTrackedAssets)) {
    g_record.outOfRange = true;
    return true;
  }

  const std::size_t index = static_cast<std::size_t>(id - kFirstId);
  ++g_record.counts[index];
  g_record.order[index] = g_record.totalLoads;
  ++g_record.totalLoads;
  return true;
}

/// How many times the asset ran its callback during the last traversal.
std::size_t load_count(AssetId id) noexcept {
  return g_record.counts[static_cast<std::size_t>(id - kFirstId)];
}

/// Position of the asset's load in the last traversal's callback order.
std::size_t load_position(AssetId id) noexcept {
  return g_record.order[static_cast<std::size_t>(id - kFirstId)];
}

/// Clears the tally between cases.
void reset_record() noexcept { g_record = LoadRecord{}; }

/// Registers an asset under a synthesized path so every case works with
/// real records rather than bare ids.
bool register_asset(MetadataStore *store, AssetId id) noexcept {
  AssetMetadata meta{};
  meta.assetId = id;
  meta.typeTag = AssetTypeTag::Mesh;
  std::snprintf(meta.filePath.data(), meta.filePath.size(),
                "assets/generated/%llu.mesh",
                static_cast<unsigned long long>(id));
  return engine::content::register_asset_metadata(store, meta);
}

} // namespace

/// Runs this executable or test program.
int main() {
  using engine::content::add_asset_dependency;
  using engine::content::clear_metadata_store;
  using engine::content::load_with_dependencies;

  // ~16 MB table: heap-allocate like production owners do.
  std::unique_ptr<MetadataStore> store(new (std::nothrow) MetadataStore());
  if (store == nullptr) {
    std::fprintf(stderr, "FAIL: store allocation\n");
    return 1;
  }

  // --- A shared dependency in a small diamond loads once. ---
  {
    const AssetId root = kFirstId;
    const AssetId left = kFirstId + 1ULL;
    const AssetId right = kFirstId + 2ULL;
    const AssetId shared = kFirstId + 3ULL;
    CHECK(register_asset(store.get(), root) &&
              register_asset(store.get(), left) &&
              register_asset(store.get(), right) &&
              register_asset(store.get(), shared),
          "diamond registers");
    CHECK(add_asset_dependency(store.get(), root, left) &&
              add_asset_dependency(store.get(), root, right) &&
              add_asset_dependency(store.get(), left, shared) &&
              add_asset_dependency(store.get(), right, shared),
          "diamond edges record");

    reset_record();
    CHECK(load_with_dependencies(store.get(), root, &record_load, nullptr),
          "the diamond loads");
    CHECK(g_record.totalLoads == 4U, "the diamond loads four assets");
    CHECK(load_count(shared) == 1U, "the shared dependency loads once");
    CHECK(load_position(shared) < load_position(left),
          "the shared dependency loads before its dependents");
    CHECK(load_position(root) == 3U, "the root loads last");
  }

  // --- A graph wider than the old 256-entry record still loads every
  // asset exactly once. ---
  //
  // The root's first eight dependencies are filler: 8 holders of 32
  // private leaves each, 264 loads that fill a 256-entry record before
  // the rest of the graph is reached. The three parents after them share
  // one leaf, so that leaf is first loaded past the point where the old
  // record stopped recording and its dependents re-loaded it.
  constexpr std::size_t kFillerHolders = 8U;
  constexpr std::size_t kFillerLeaves = AssetMetadata::kMaxDependencies;
  constexpr std::size_t kLateParents = 3U;
  constexpr std::size_t kWideTotal = 1U + kFillerHolders +
                                     (kFillerHolders * kFillerLeaves) +
                                     kLateParents + 1U;
  const AssetId wideRoot = kFirstId + 10ULL;
  const AssetId sharedLeaf = kFirstId + 11ULL;
  const AssetId firstHolder = kFirstId + 12ULL;
  const AssetId firstParent = firstHolder + kFillerHolders;
  const AssetId firstFillerLeaf = firstParent + kLateParents;
  {
    clear_metadata_store(store.get());
    CHECK(register_asset(store.get(), wideRoot) &&
              register_asset(store.get(), sharedLeaf),
          "wide graph root and shared leaf register");

    for (std::size_t holder = 0U; holder < kFillerHolders; ++holder) {
      const AssetId holderId = firstHolder + static_cast<AssetId>(holder);
      CHECK(register_asset(store.get(), holderId), "filler holder registers");
      CHECK(add_asset_dependency(store.get(), wideRoot, holderId),
            "root depends on the filler holder");

      for (std::size_t leaf = 0U; leaf < kFillerLeaves; ++leaf) {
        const AssetId leafId =
            firstFillerLeaf +
            static_cast<AssetId>((holder * kFillerLeaves) + leaf);
        CHECK(register_asset(store.get(), leafId), "filler leaf registers");
        CHECK(add_asset_dependency(store.get(), holderId, leafId),
              "filler holder depends on its private leaf");
      }
    }

    for (std::size_t parent = 0U; parent < kLateParents; ++parent) {
      const AssetId parentId = firstParent + static_cast<AssetId>(parent);
      CHECK(register_asset(store.get(), parentId), "late parent registers");
      CHECK(add_asset_dependency(store.get(), wideRoot, parentId),
            "root depends on the late parent");
      CHECK(add_asset_dependency(store.get(), parentId, sharedLeaf),
            "late parent depends on the shared leaf");
    }

    reset_record();
    CHECK(load_with_dependencies(store.get(), wideRoot, &record_load, nullptr),
          "the wide graph loads");
    CHECK(!g_record.outOfRange, "every load stays inside the tracked range");
    CHECK(g_record.totalLoads == kWideTotal,
          "the wide graph runs one callback per distinct asset");
    CHECK(load_count(sharedLeaf) == 1U,
          "a leaf first reached past the 256th load still loads once");
    CHECK(load_position(sharedLeaf) >= 256U,
          "the shared leaf is reached after the old record would have filled");

    std::size_t repeated = 0U;
    for (std::size_t holder = 0U; holder < kFillerHolders; ++holder) {
      const AssetId holderId = firstHolder + static_cast<AssetId>(holder);
      if ((load_count(holderId) != 1U) ||
          (load_position(holderId) > load_position(wideRoot))) {
        ++repeated;
      }

      for (std::size_t leaf = 0U; leaf < kFillerLeaves; ++leaf) {
        const AssetId leafId =
            firstFillerLeaf +
            static_cast<AssetId>((holder * kFillerLeaves) + leaf);
        if ((load_count(leafId) != 1U) ||
            (load_position(leafId) > load_position(holderId))) {
          ++repeated;
        }
      }
    }
    for (std::size_t parent = 0U; parent < kLateParents; ++parent) {
      const AssetId parentId = firstParent + static_cast<AssetId>(parent);
      if ((load_count(parentId) != 1U) ||
          (load_position(parentId) > load_position(wideRoot)) ||
          (load_position(sharedLeaf) > load_position(parentId))) {
        ++repeated;
      }
    }
    CHECK(repeated == 0U,
          "every asset in the wide graph loads once, dependencies first");
    CHECK(load_position(wideRoot) == (kWideTotal - 1U),
          "the wide graph's root loads last");
  }

  // --- The same traversal repeats cleanly on the same store. ---
  {
    reset_record();
    CHECK(load_with_dependencies(store.get(), wideRoot, &record_load, nullptr),
          "the wide graph loads a second time");
    CHECK(g_record.totalLoads == kWideTotal,
          "a repeated traversal starts from an empty visited set");
    CHECK(load_count(sharedLeaf) == 1U,
          "the shared leaf still loads exactly once on the second traversal");
  }

  // --- A cycle is still rejected behind many already-loaded assets. ---
  {
    CHECK(add_asset_dependency(store.get(), sharedLeaf, wideRoot),
          "the back edge records");
    reset_record();
    CHECK(!load_with_dependencies(store.get(), wideRoot, &record_load, nullptr),
          "a cycle behind a wide graph is rejected");
  }

  // --- An id the store holds no record for is still deduplicated. ---
  {
    clear_metadata_store(store.get());
    const AssetId root = kFirstId + 400ULL;
    const AssetId danglingDep = kFirstId + 401ULL;
    CHECK(register_asset(store.get(), root), "dangling-dep root registers");

    for (std::size_t mid = 0U; mid < 3U; ++mid) {
      const AssetId midId = kFirstId + 402ULL + static_cast<AssetId>(mid);
      CHECK(register_asset(store.get(), midId),
            "dangling-dep mid-level asset registers");
      CHECK(add_asset_dependency(store.get(), root, midId),
            "root depends on the mid-level asset");
      CHECK(add_asset_dependency(store.get(), midId, danglingDep),
            "mid-level asset depends on the unregistered id");
    }

    reset_record();
    CHECK(load_with_dependencies(store.get(), root, &record_load, nullptr),
          "a graph with an unregistered dependency loads");
    CHECK(g_record.totalLoads == 5U,
          "root, three mid-level assets and the unregistered id load");
    CHECK(load_count(danglingDep) == 1U,
          "an unregistered dependency shared by three assets loads once");
  }

  // --- A leaf with no dependencies loads exactly once. ---
  {
    clear_metadata_store(store.get());
    const AssetId lone = kFirstId + 450ULL;
    CHECK(register_asset(store.get(), lone), "lone asset registers");

    reset_record();
    CHECK(load_with_dependencies(store.get(), lone, &record_load, nullptr),
          "an asset with no dependencies loads");
    CHECK(g_record.totalLoads == 1U, "it runs exactly one callback");
  }

  // --- More unregistered ids than the traversal can remember is reported,
  // not met by silently loading a shared id twice. ---
  {
    clear_metadata_store(store.get());
    const AssetId root = kFirstId + 500ULL;
    CHECK(register_asset(store.get(), root), "overflow root registers");

    // Nine holders of 32 dependency edges each reference 288 distinct ids
    // with no metadata record, past the 256 the traversal can hold.
    AssetId nextDangling = kFirstId + 600ULL;
    for (std::size_t holder = 0U; holder < 9U; ++holder) {
      const AssetId holderId = kFirstId + 501ULL + static_cast<AssetId>(holder);
      CHECK(register_asset(store.get(), holderId), "edge holder registers");
      CHECK(add_asset_dependency(store.get(), root, holderId),
            "root depends on the edge holder");
      for (std::size_t edge = 0U; edge < AssetMetadata::kMaxDependencies;
           ++edge) {
        CHECK(add_asset_dependency(store.get(), holderId, nextDangling),
              "unregistered dependency edge records");
        ++nextDangling;
      }
    }

    reset_record();
    CHECK(!load_with_dependencies(store.get(), root, &record_load, nullptr),
          "exhausting the unregistered-id record is reported as a failure");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }

  std::puts("content_dependency_dedup_test passed");
  return 0;
}
