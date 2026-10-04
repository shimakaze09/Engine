// Implements the importer registry and the texture importer, whose cook is
// the browser thumbnail: the engine reads a texture's source as authored
// and applies its settings at load, so nothing else is cooked from it.

#include "importer.h"

#include <array>

namespace engine::tools {
namespace {

/// No texture setting changes the thumbnail, which shows the source.
std::uint64_t
hash_no_settings(const content::ResolvedImportSettings & /*settings*/) {
  return 0ULL;
}

CookResult cook_texture_thumbnail(const CookRequest &request) {
  CookResult result{};
  if (!generate_texture_thumbnail(request.inputPath, request.outputPath)) {
    result.exitCode = 14;
  }
  return result;
}

const Importer kTextureImporter{"texture thumbnail",
                                content::AssetTypeTag::Texture,
                                "texture-thumbnail-1",
                                &hash_no_settings,
                                nullptr,
                                &cook_texture_thumbnail,
                                false};

/// Every importer; a type appears at most once.
constexpr std::array<const Importer *, 2U> kImporters{&kGltfMeshImporter,
                                                      &kTextureImporter};

} // namespace

const Importer *find_importer(content::AssetTypeTag tag) noexcept {
  for (const Importer *importer : kImporters) {
    if (importer->claims == tag) {
      return importer;
    }
  }
  return nullptr;
}

} // namespace engine::tools
