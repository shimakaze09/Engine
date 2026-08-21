// Implements the packer's bgfx shader cook (#138 Phase C): reads the
// shader manifest (sources, stages, output stems, variant define sets),
// invokes bgfx shaderc per variant and platform profile, stages each
// binary through an atomic replace, and commits the whole output set
// under one cook stamp so interruption can never certify a mixed
// generation (#211). Inputs are digested (manifest, sources, varying
// table, shaderc's shared include headers) so should_repack skips
// byte-identical cooks and any input edit recooks.

#include "shader_cook.h"

#include "packer_shared.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "engine/core/atomic_file.h"
#include "engine/core/json.h"

namespace {

constexpr std::uint64_t kFnv64Offset = 14695981039346656037ULL;
constexpr std::uint64_t kFnv64Prime = 1099511628211ULL;

/// One engine profile tag with its shaderc platform/profile arguments.
struct ShaderProfile final {
  const char *tag;
  const char *platform;
  const char *profile;
};

constexpr ShaderProfile kKnownProfiles[] = {
    {"glsl", "linux", "440"},
    {"essl", "android", "300_es"},
    {"spirv", "linux", "spirv"},
    {"metal", "osx", "metal"},
};

/// One manifest entry: a .sc source, its stage, the runtime output stem,
/// and its variant define sets.
struct ShaderEntry final {
  std::string source{};
  bool isVertex = false;
  std::string output{};
  std::vector<std::vector<std::string>> variants{};
};

/// FNV-1a over a string, continuing from a running hash.
std::uint64_t fnv_append(std::uint64_t hash, const char *text) {
  for (const char *c = text; *c != '\0'; ++c) {
    hash ^= static_cast<std::uint8_t>(*c);
    hash *= kFnv64Prime;
  }
  hash ^= 0xFFU; // separator so field boundaries stay distinct
  hash *= kFnv64Prime;
  return hash;
}

/// Deterministic variant key: sorted defines joined with '-', or
/// "default" for the empty set. The runtime derives the same key from a
/// requested define set to locate the cooked binary.
std::string variant_key(std::vector<std::string> defines) {
  if (defines.empty()) {
    return "default";
  }
  std::sort(defines.begin(), defines.end());
  std::string key;
  for (std::size_t i = 0U; i < defines.size(); ++i) {
    if (i > 0U) {
      key += "-";
    }
    key += defines[i];
  }
  return key;
}

/// Reads the manifest JSON into shader entries; false on parse or shape
/// errors (logged).
bool read_manifest(const char *manifestPath,
                   std::vector<ShaderEntry> *outEntries) {
  FILE *file = std::fopen(manifestPath, "rb");
  if (file == nullptr) {
    std::fprintf(stderr, "shader cook: cannot open manifest %s\n",
                 manifestPath);
    return false;
  }
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  std::string text(static_cast<std::size_t>(size > 0 ? size : 0), '\0');
  const std::size_t read =
      (size > 0) ? std::fread(text.data(), 1U, text.size(), file) : 0U;
  std::fclose(file);
  if (read != text.size()) {
    std::fprintf(stderr, "shader cook: failed reading manifest %s\n",
                 manifestPath);
    return false;
  }

  engine::core::JsonParser parser;
  if (!parser.parse(text.c_str(), text.size()) || (parser.root() == nullptr)) {
    std::fprintf(stderr, "shader cook: manifest %s is not valid JSON\n",
                 manifestPath);
    return false;
  }
  const engine::core::JsonValue *shaders =
      parser.get_object_field(*parser.root(), "shaders");
  if (shaders == nullptr) {
    std::fprintf(stderr, "shader cook: manifest missing \"shaders\" array\n");
    return false;
  }
  const std::size_t count = parser.array_size(*shaders);
  if (count == 0U) {
    std::fprintf(stderr, "shader cook: manifest lists no shaders\n");
    return false;
  }
  for (std::size_t i = 0U; i < count; ++i) {
    const engine::core::JsonValue *entry =
        parser.get_array_element(*shaders, i);
    if (entry == nullptr) {
      return false;
    }
    ShaderEntry shader{};
    char buffer[256] = {};
    const engine::core::JsonValue *field =
        parser.get_object_field(*entry, "source");
    if ((field == nullptr) ||
        !parser.copy_string(*field, buffer, sizeof(buffer))) {
      std::fprintf(stderr, "shader cook: entry %zu missing source\n", i);
      return false;
    }
    shader.source = buffer;
    field = parser.get_object_field(*entry, "type");
    if ((field == nullptr) ||
        !parser.copy_string(*field, buffer, sizeof(buffer))) {
      std::fprintf(stderr, "shader cook: entry %zu missing type\n", i);
      return false;
    }
    if (std::strcmp(buffer, "vertex") == 0) {
      shader.isVertex = true;
    } else if (std::strcmp(buffer, "fragment") == 0) {
      shader.isVertex = false;
    } else {
      std::fprintf(stderr, "shader cook: entry %zu type must be vertex or "
                           "fragment\n",
                   i);
      return false;
    }
    field = parser.get_object_field(*entry, "output");
    if ((field == nullptr) ||
        !parser.copy_string(*field, buffer, sizeof(buffer))) {
      std::fprintf(stderr, "shader cook: entry %zu missing output\n", i);
      return false;
    }
    shader.output = buffer;
    const engine::core::JsonValue *variants =
        parser.get_object_field(*entry, "variants");
    if (variants == nullptr) {
      std::fprintf(stderr, "shader cook: entry %zu missing variants\n", i);
      return false;
    }
    const std::size_t variantCount = parser.array_size(*variants);
    for (std::size_t v = 0U; v < variantCount; ++v) {
      const engine::core::JsonValue *variant =
          parser.get_array_element(*variants, v);
      if (variant == nullptr) {
        return false;
      }
      std::vector<std::string> defines;
      const std::size_t defineCount = parser.array_size(*variant);
      for (std::size_t d = 0U; d < defineCount; ++d) {
        const engine::core::JsonValue *define =
            parser.get_array_element(*variant, d);
        if ((define == nullptr) ||
            !parser.copy_string(*define, buffer, sizeof(buffer))) {
          std::fprintf(stderr,
                       "shader cook: entry %zu variant %zu malformed\n", i,
                       v);
          return false;
        }
        defines.emplace_back(buffer);
      }
      shader.variants.push_back(std::move(defines));
    }
    if (shader.variants.empty()) {
      std::fprintf(stderr, "shader cook: entry %zu has no variants\n", i);
      return false;
    }
    outEntries->push_back(std::move(shader));
  }
  return true;
}

/// Invokes shaderc for one source/variant/profile into a staged sibling
/// and atomically replaces the final output; false on any failure with
/// the stage cleaned up.
bool cook_one(const std::string &shadercPath, const std::string &sourcePath,
              const std::string &varyingPath, const std::string &includeDir,
              bool isVertex, const std::vector<std::string> &defines,
              const ShaderProfile &profile, const std::string &finalPath) {
  const std::string stagedPath = finalPath + ".cooking";
  std::string command = "\"" + shadercPath + "\" -f \"" + sourcePath +
                        "\" -o \"" + stagedPath + "\" --type " +
                        (isVertex ? "v" : "f") + " --platform " +
                        profile.platform + " -p " + profile.profile +
                        " -i \"" + includeDir + "\" --varyingdef \"" +
                        varyingPath + "\"";
  for (const std::string &define : defines) {
    command += " --define " + define;
  }
#ifdef _WIN32
  // cmd.exe strips the outer quote pair from the whole command line.
  command = "\"" + command + "\"";
#else
  command += " 2>/dev/null";
#endif
  const int exitCode = std::system(command.c_str());
  bool ok = (exitCode == 0);
  std::vector<char> bytes;
  if (ok) {
    FILE *staged = std::fopen(stagedPath.c_str(), "rb");
    ok = (staged != nullptr);
    if (ok) {
      std::fseek(staged, 0, SEEK_END);
      const long size = std::ftell(staged);
      std::fseek(staged, 0, SEEK_SET);
      ok = (size > 0);
      if (ok) {
        bytes.resize(static_cast<std::size_t>(size));
        ok = (std::fread(bytes.data(), 1U, bytes.size(), staged) ==
              bytes.size());
      }
      std::fclose(staged);
    }
  }
  std::error_code ignored;
  std::filesystem::remove(stagedPath, ignored);
  if (!ok) {
    std::fprintf(stderr, "shader cook: shaderc failed for %s (%s)\n",
                 sourcePath.c_str(), profile.tag);
    return false;
  }
  if (!engine::core::atomic_write_file(finalPath.c_str(), bytes.data(),
                                       bytes.size())) {
    std::fprintf(stderr, "shader cook: cannot commit %s\n",
                 finalPath.c_str());
    return false;
  }
  return true;
}

} // namespace

int run_shader_cook(int argc, char **argv) {
  const char *manifestPath = nullptr;
  const char *outDir = nullptr;
  const char *shadercPath = nullptr;
  const char *includeDir = nullptr;
  std::string profilesCsv = "glsl,essl,spirv";
  const char *platformTag = kCookPlatformTag;
  bool force = false;

  for (int i = 1; i < argc; ++i) {
    const auto takes_value = [&](const char *flag,
                                 const char **out) -> bool {
      if (std::strcmp(argv[i], flag) == 0) {
        if ((i + 1) >= argc) {
          std::fprintf(stderr, "shader cook: %s needs a value\n", flag);
          return false;
        }
        ++i;
        *out = argv[i];
        return true;
      }
      return false;
    };
    const char *value = nullptr;
    if (takes_value("--shader-manifest", &manifestPath) ||
        takes_value("--shader-out", &outDir) ||
        takes_value("--shaderc", &shadercPath) ||
        takes_value("--shader-include", &includeDir) ||
        takes_value("--platform", &platformTag)) {
      continue;
    }
    if (takes_value("--profiles", &value)) {
      profilesCsv = value;
      continue;
    }
    if (std::strcmp(argv[i], "--force") == 0) {
      force = true;
      continue;
    }
    std::fprintf(stderr, "shader cook: unknown argument %s\n", argv[i]);
    return 1;
  }
  if ((manifestPath == nullptr) || (outDir == nullptr) ||
      (shadercPath == nullptr) || (includeDir == nullptr)) {
    std::fprintf(stderr,
                 "usage: asset_packer --shader-manifest <shaders.json> "
                 "--shader-out <dir> --shaderc <path> --shader-include "
                 "<bgfx src dir> [--profiles glsl,essl,spirv] [--force] "
                 "[--platform <tag>]\n");
    return 1;
  }
  if (!is_valid_platform_tag(platformTag)) {
    std::fprintf(stderr, "shader cook: invalid platform tag\n");
    return 1;
  }

  std::vector<ShaderProfile> profiles;
  {
    std::string csv = profilesCsv;
    std::size_t start = 0U;
    while (start <= csv.size()) {
      std::size_t end = csv.find(',', start);
      if (end == std::string::npos) {
        end = csv.size();
      }
      const std::string tag = csv.substr(start, end - start);
      start = end + 1U;
      if (tag.empty()) {
        continue;
      }
      bool known = false;
      for (const ShaderProfile &profile : kKnownProfiles) {
        if (tag == profile.tag) {
          profiles.push_back(profile);
          known = true;
          break;
        }
      }
      if (!known) {
        std::fprintf(stderr, "shader cook: unknown profile %s\n",
                     tag.c_str());
        return 1;
      }
    }
  }
  if (profiles.empty()) {
    std::fprintf(stderr, "shader cook: no profiles requested\n");
    return 1;
  }

  std::vector<ShaderEntry> entries;
  if (!read_manifest(manifestPath, &entries)) {
    return 1;
  }

  const std::filesystem::path manifestDir =
      std::filesystem::path(manifestPath).parent_path();
  const std::string varyingPath =
      (manifestDir / "varying.def.sc").string();

  // Inputs: manifest + every source + varying table + shaderc's shared
  // include headers. Any content change recooks the whole set.
  std::vector<std::string> dependencyPaths = {
      manifestPath, varyingPath,
      (std::filesystem::path(includeDir) / "bgfx_shader.sh").string(),
      (std::filesystem::path(includeDir) / "bgfx_compute.sh").string()};
  for (const ShaderEntry &entry : entries) {
    dependencyPaths.push_back((manifestDir / entry.source).string());
  }
  std::vector<DependencyDigest> digests;
  if (!build_dependency_digests(dependencyPaths, &digests)) {
    std::fprintf(stderr, "shader cook: missing input file\n");
    return 1;
  }
  sort_dependency_digests(digests);

  bool sourceOk = false;
  const std::uint64_t sourceHash = hash_file_contents(manifestPath, &sourceOk);
  if (!sourceOk) {
    return 1;
  }
  // Settings analog: the requested profile set and every variant define
  // participate in the cook key, so a profile or variant change recooks.
  std::uint64_t settingsHash = kFnv64Offset;
  settingsHash = fnv_append(settingsHash, profilesCsv.c_str());
  for (const ShaderEntry &entry : entries) {
    settingsHash = fnv_append(settingsHash, entry.output.c_str());
    for (const std::vector<std::string> &variant : entry.variants) {
      settingsHash =
          fnv_append(settingsHash, variant_key(variant).c_str());
    }
  }

  const std::string stampBase =
      (std::filesystem::path(outDir) / "bgfx_shaders").string();
  std::vector<std::string> outputs;
  for (const ShaderEntry &entry : entries) {
    for (const std::vector<std::string> &variant : entry.variants) {
      for (const ShaderProfile &profile : profiles) {
        outputs.push_back((std::filesystem::path(outDir) /
                           (entry.output + "." + variant_key(variant) +
                            "." + profile.tag + ".bin"))
                              .string());
      }
    }
  }

  if (!force && !should_repack(stampBase.c_str(), sourceHash, digests,
                               settingsHash, platformTag)) {
    std::printf("shader cook: up to date (%zu outputs)\n", outputs.size());
    return 0;
  }

  if (!ensure_directory_exists(outDir)) {
    std::fprintf(stderr, "shader cook: cannot create %s\n", outDir);
    return 1;
  }

  std::size_t outputIndex = 0U;
  for (const ShaderEntry &entry : entries) {
    for (const std::vector<std::string> &variant : entry.variants) {
      for (const ShaderProfile &profile : profiles) {
        const std::string &finalPath = outputs[outputIndex];
        ++outputIndex;
        if (!cook_one(shadercPath, (manifestDir / entry.source).string(),
                      varyingPath, includeDir, entry.isVertex, variant,
                      profile, finalPath)) {
          // No stamp: the previous stamp (if any) still certifies the
          // previous complete generation; a partial new one never
          // becomes certified.
          return 1;
        }
      }
    }
  }

  // The stamp anchors on its output file, so the cook's primary output
  // is a deterministic index of the committed binaries at the stamp
  // base path.
  std::string index = "# bgfx shader cook index (profiles: " + profilesCsv +
                      ")\n";
  for (const std::string &output : outputs) {
    index += std::filesystem::path(output).filename().string() + "\n";
  }
  if (!engine::core::atomic_write_file(stampBase.c_str(), index.data(),
                                       index.size())) {
    std::fprintf(stderr, "shader cook: index write failed\n");
    return 1;
  }

  if (!remove_stale_outputs(stampBase.c_str(), outputs)) {
    return 1;
  }
  if (!write_cook_stamp(stampBase.c_str(), sourceHash, digests, settingsHash,
                        platformTag, outputs)) {
    std::fprintf(stderr, "shader cook: stamp write failed\n");
    return 1;
  }
  std::printf("shader cook: %zu outputs committed\n", outputs.size());
  return 0;
}
