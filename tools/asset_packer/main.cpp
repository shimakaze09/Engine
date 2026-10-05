// The asset packer's command line: cooks one source through the importer
// that claims its type (importer.h) and certifies the outputs with a cook
// stamp, or runs the shader cook or the identity migration.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "engine/content/asset_type_table.h"
#include "engine/content/import_settings_resolve.h"
#include "engine/core/atomic_file.h"

#include "generated_manifest.h"
#include "importer.h"
#include "packer_shared.h"
#include "shader_cook.h"

namespace {

void print_usage() {
  std::fprintf(stderr,
               "usage: asset_packer <source> <output> "
               "[--dep <dependency_path>]... "
               "[--force] [--verify] [--sweep-orphans] "
               "[--platform <tag>]\n"
               "   or: asset_packer --shader-manifest <shaders.manifest> "
               "--shader-out <dir> --shaderc <path> "
               "--shader-include <dir> [--profiles <csv>] [--force]\n"
               "   or: asset_packer --init-meta <assets-dir>\n");
}

} // namespace

bool ensure_directory_exists(const char *dirPath) {
  if (dirPath == nullptr) {
    return false;
  }
  // Every missing level, durably, through core's one directory-creation
  // path: the old single mkdir failed for `build/new/nested/cooked`.
  return engine::core::create_directories_durably(dirPath);
}

/// Runs this executable or test program.
int main(int argc, char **argv) {
  // The bgfx shader cook mode has its own argument shape;
  // dispatch before the glTF flow's positional parsing.
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--shader-manifest") == 0) {
      return run_shader_cook(argc, argv);
    }
    if (std::strcmp(argv[i], "--init-meta") == 0) {
      return run_init_meta(argc, argv);
    }
  }
  if (argc < 3) {
    print_usage();
    return 1;
  }

  const char *inputPath = argv[1];
  const char *outputPath = argv[2];

  bool forceRepack = false;
  bool verifyOutputs = false;
  bool sweepOrphans = false;
  const char *platformTag = kAssetCookPlatformTag;
  std::vector<std::string> dependencyPaths{};
  for (int i = 3; i < argc; ++i) {
    if (std::strcmp(argv[i], "--dep") == 0) {
      if ((i + 1) >= argc) {
        print_usage();
        return 8;
      }
      dependencyPaths.emplace_back(argv[i + 1]);
      ++i;
      continue;
    }

    if (std::strcmp(argv[i], "--force") == 0) {
      forceRepack = true;
      continue;
    }

    if (std::strcmp(argv[i], "--verify") == 0) {
      verifyOutputs = true;
      continue;
    }

    if (std::strcmp(argv[i], "--sweep-orphans") == 0) {
      sweepOrphans = true;
      continue;
    }

    if (std::strcmp(argv[i], "--platform") == 0) {
      if ((i + 1) >= argc) {
        print_usage();
        return 8;
      }
      platformTag = argv[i + 1];
      ++i;
      continue;
    }

    print_usage();
    return 9;
  }

  if (!is_valid_platform_tag(platformTag)) {
    std::fprintf(stderr,
                 "error: invalid platform tag (single token, <64 chars)\n");
    return 9;
  }

  // The importer is chosen by the type the asset type table gives the
  // source, the same classification the catalog makes.
  const engine::content::AssetClassification input =
      engine::content::classify_asset_path(inputPath);
  const engine::tools::Importer *importer =
      input.source ? engine::tools::find_importer(input.tag) : nullptr;
  if (importer == nullptr) {
    std::fprintf(stderr,
                 "error: no importer cooks %s: it is not the source of a "
                 "cooked asset type\n",
                 inputPath);
    return 2;
  }

  bool sourceHashOk = false;
  const std::uint64_t sourceHash = hash_file_contents(inputPath, &sourceHashOk);
  if (!sourceHashOk) {
    std::fprintf(stderr, "error: failed to read input file for hashing\n");
    return 10;
  }

  std::vector<DependencyDigest> dependencyDigests{};
  if (!build_dependency_digests(dependencyPaths, &dependencyDigests)) {
    return 11;
  }
  if (importer->discover_dependencies != nullptr) {
    importer->discover_dependencies(inputPath, hash_path_to_asset_id(inputPath),
                                    &dependencyDigests);
  }

  // From the source's authored sidecar, or an enclosing folder's. The
  // cooked record is derived and regenerable, so it can never be where an
  // author's settings live.
  engine::content::ResolvedImportSettings settings{};
  if (!engine::content::resolve_import_settings(inputPath, &settings)) {
    // Cooking at the defaults would throw away what the author typed and
    // look like it worked.
    std::fprintf(stderr,
                 "error: the source's import settings are unknown: a "
                 "sidecar could not be read; fix or remove it: %s.meta\n",
                 settings.unreadable);
    return 22;
  }
  // The cook key pairs the settings that change the cooked bytes with the
  // importer's logic revision, so a logic change recooks (and re-renders
  // any thumbnail derived from the cooked data) without a settings edit.
  const std::uint64_t settingsKey = cook_settings_key(
      importer->hash_settings(settings), importer->logicRevision);

  // Sort dependencies by path for deterministic output.
  sort_dependency_digests(dependencyDigests);

  // A generated source (or dependency) is cooked only when its
  // directory manifest certifies these exact bytes; the generators write
  // that manifest last, so a source that disagrees with it belongs to an
  // interrupted publish and would cook as a mixed generation.
  {
    char refusal[1024] = {};
    if (!generated_source_certified(inputPath, sourceHash, refusal,
                                    sizeof(refusal))) {
      std::fprintf(stderr, "error: %s\n", refusal);
      return 21;
    }
    for (const DependencyDigest &dependency : dependencyDigests) {
      if (!generated_source_certified(dependency.path.c_str(),
                                      dependency.hash, refusal,
                                      sizeof(refusal))) {
        std::fprintf(stderr, "error: %s\n", refusal);
        return 21;
      }
    }
  }

  if (!forceRepack && importer->stamped &&
      !should_repack(outputPath, sourceHash, dependencyDigests, settingsKey,
                     platformTag, verifyOutputs)) {
    std::printf("asset up-to-date; skipped recook: %s\n", outputPath);
    if (sweepOrphans && !sweep_orphan_outputs(outputPath)) {
      return 19;
    }
    return 0;
  }

  engine::tools::CookRequest request{};
  request.inputPath = inputPath;
  request.outputPath = outputPath;
  request.settings = &settings;
  request.sourceHash = sourceHash;
  request.dependencies = &dependencyDigests;
  request.settingsKey = settingsKey;
  const engine::tools::CookResult cooked = importer->cook(request);
  if (cooked.exitCode != 0) {
    return cooked.exitCode;
  }
  if (!importer->stamped) {
    return 0;
  }

  if (!remove_stale_outputs(outputPath, cooked.outputs)) {
    return 18;
  }

  // The stamp is the cook's commit marker: written only after every
  // output above landed (and stale outputs of the previous manifest were
  // retired), so any interruption leaves no fresh stamp and the next run
  // recooks the full output set.
  if (!write_cook_stamp(outputPath, inputPath, sourceHash, dependencyDigests,
                        settingsKey, platformTag, cooked.outputs)) {
    std::fprintf(stderr, "error: failed to write cook stamp\n");
    return 13;
  }

  if (sweepOrphans && !sweep_orphan_outputs(outputPath)) {
    return 19;
  }

  std::printf("%s\n", cooked.summary.c_str());
  return 0;
}
