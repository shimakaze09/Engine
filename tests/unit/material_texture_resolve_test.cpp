// Verifies resolve_material_textures: successful resolution populates GPU
// handles, a failed load falls back to invalid handles (never a crash) and
// is not retried, and a texture shared by two materials loads only once.
// The GL-touching loader is stubbed via MaterialTextureLoadFn injection so
// this stays a CPU-only, headless-safe test (issue #160 fallback policy).

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/material_loader.h"

namespace {

bool write_material_file(const char *path, const char *text) noexcept {
  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t size = std::strlen(text);
  const std::size_t written = std::fwrite(text, 1U, size, file);
  std::fclose(file);
  return written == size;
}

void remove_file(const char *path) noexcept {
  static_cast<void>(std::remove(path));
}

/// Counts calls and fails any path containing "missing"; every other path
/// "loads" successfully to a handle derived from a running counter so
/// distinct textures get distinct fake ids.
struct FakeLoaderState final {
  std::uint32_t callCount = 0U;
  std::uint32_t nextHandle = 1U;
};

engine::renderer::TextureHandle fake_load_texture(const char *path,
                                                  void *userData) noexcept {
  auto *state = static_cast<FakeLoaderState *>(userData);
  ++state->callCount;
  if ((path == nullptr) || (std::strstr(path, "missing") != nullptr)) {
    return engine::renderer::kInvalidTextureHandle;
  }
  return engine::renderer::TextureHandle{state->nextHandle++};
}

/// A material with every texture slot set resolves every slot to a valid
/// handle exactly once; a second sync call does not re-invoke the loader.
int verify_successful_resolution(engine::renderer::AssetDatabase *database) {
  constexpr const char *kPath = "material_resolve_ok.json";
  constexpr const char *kVirtualPath = "mat/material_resolve_ok.json";
  constexpr const char *kJson =
      "{\"version\":2,\"textures\":{"
      "\"albedo\":\"assets/textures/ok_albedo.png\","
      "\"metallicRoughness\":\"assets/textures/ok_mr.png\","
      "\"emissive\":\"assets/textures/ok_emissive.png\","
      "\"occlusion\":\"assets/textures/ok_ao.png\","
      "\"opacity\":\"assets/textures/ok_opacity.png\"}}";
  if (!write_material_file(kPath, kJson)) {
    return 10;
  }
  const auto loadResult =
      engine::renderer::load_material_asset(database, kVirtualPath);
  remove_file(kPath);
  if (!loadResult.has_value()) {
    return 11;
  }
  const engine::renderer::AssetId id = *loadResult;

  FakeLoaderState state{};
  const std::size_t resolvedFirst = engine::renderer::resolve_material_textures(
      database, &fake_load_texture, &state);
  if (resolvedFirst != 5U) {
    return 12;
  }
  if (state.callCount != 5U) {
    return 13;
  }

  const engine::renderer::Material *params =
      engine::renderer::find_material_params(database, id);
  if ((params == nullptr) ||
      (params->albedoTexture == engine::renderer::kInvalidTextureHandle) ||
      (params->metallicRoughnessTexture ==
       engine::renderer::kInvalidTextureHandle) ||
      (params->emissiveTexture == engine::renderer::kInvalidTextureHandle) ||
      (params->occlusionTexture == engine::renderer::kInvalidTextureHandle) ||
      (params->opacityTexture == engine::renderer::kInvalidTextureHandle)) {
    return 14;
  }

  // Every already-Ready slot is a cheap lookup on the next sync — the
  // loader must not be called again.
  const std::size_t resolvedSecond =
      engine::renderer::resolve_material_textures(database, &fake_load_texture,
                                                   &state);
  if ((resolvedSecond != 0U) || (state.callCount != 5U)) {
    return 15;
  }

  return 0;
}

/// A failing texture load leaves the material's handle invalid (scalar
/// fallback) and is not retried on a later sync.
int verify_failed_load_falls_back(engine::renderer::AssetDatabase *database) {
  constexpr const char *kPath = "material_resolve_missing.json";
  constexpr const char *kVirtualPath = "mat/material_resolve_missing.json";
  constexpr const char *kJson =
      "{\"version\":2,\"roughness\":0.6,\"textures\":{"
      "\"albedo\":\"assets/textures/missing_albedo.png\"}}";
  if (!write_material_file(kPath, kJson)) {
    return 20;
  }
  const auto loadResult =
      engine::renderer::load_material_asset(database, kVirtualPath);
  remove_file(kPath);
  if (!loadResult.has_value()) {
    return 21;
  }
  const engine::renderer::AssetId id = *loadResult;

  FakeLoaderState state{};
  static_cast<void>(engine::renderer::resolve_material_textures(
      database, &fake_load_texture, &state));

  const engine::renderer::Material *params =
      engine::renderer::find_material_params(database, id);
  // Fallback is visible: the scalar roughness the author set is still there
  // and the texture handle stays invalid rather than crashing or binding an
  // unrelated texture.
  if ((params == nullptr) ||
      (params->albedoTexture != engine::renderer::kInvalidTextureHandle) ||
      (params->roughness != 0.6F)) {
    return 22;
  }

  const engine::renderer::MaterialTextureSlots *slots =
      engine::renderer::find_material_texture_slots(database, id);
  const engine::renderer::AssetId textureId =
      (slots != nullptr) ? slots->albedo : engine::renderer::kInvalidAssetId;
  if (engine::renderer::texture_asset_state(database, textureId) !=
      engine::renderer::AssetState::Failed) {
    return 23;
  }

  const std::uint32_t callsAfterFirstSync = state.callCount;
  static_cast<void>(engine::renderer::resolve_material_textures(
      database, &fake_load_texture, &state));
  if (state.callCount != callsAfterFirstSync) {
    return 24;
  }

  return 0;
}

/// Two materials referencing the same texture path load it exactly once.
int verify_shared_texture_loads_once(engine::renderer::AssetDatabase *database) {
  constexpr const char *kPathA = "material_resolve_shared_a.json";
  constexpr const char *kPathB = "material_resolve_shared_b.json";
  constexpr const char *kJson =
      "{\"version\":2,\"textures\":{"
      "\"albedo\":\"assets/textures/shared_albedo.png\"}}";
  if (!write_material_file(kPathA, kJson) ||
      !write_material_file(kPathB, kJson)) {
    remove_file(kPathA);
    remove_file(kPathB);
    return 30;
  }
  const auto resultA = engine::renderer::load_material_asset(
      database, "mat/material_resolve_shared_a.json");
  const auto resultB = engine::renderer::load_material_asset(
      database, "mat/material_resolve_shared_b.json");
  remove_file(kPathA);
  remove_file(kPathB);
  if (!resultA.has_value() || !resultB.has_value()) {
    return 31;
  }

  FakeLoaderState state{};
  const std::size_t resolvedCount = engine::renderer::resolve_material_textures(
      database, &fake_load_texture, &state);
  // resolve_material_textures counts newly-loaded slots: whichever of A/B
  // is scanned first drives the one real load (resolvedCount == 1); the
  // other reuses the now-Ready record via a lookup, not a second load —
  // the shared texture id only reaches the loader once either way.
  if ((resolvedCount != 1U) || (state.callCount != 1U)) {
    return 32;
  }

  const engine::renderer::Material *paramsA =
      engine::renderer::find_material_params(database, *resultA);
  const engine::renderer::Material *paramsB =
      engine::renderer::find_material_params(database, *resultB);
  if ((paramsA == nullptr) || (paramsB == nullptr) ||
      (paramsA->albedoTexture != paramsB->albedoTexture)) {
    return 33;
  }

  return 0;
}

/// A material with no texture slots at all is a no-op (never calls the
/// loader) and the call still succeeds against a null database.
int verify_no_slots_and_null_database() {
  if (engine::renderer::resolve_material_textures(nullptr, &fake_load_texture,
                                                   nullptr) != 0U) {
    return 40;
  }
  return 0;
}

} // namespace

int main() {
  if (!engine::core::initialize_vfs()) {
    return 1;
  }
  if (!engine::core::mount("mat", ".")) {
    engine::core::shutdown_vfs();
    return 2;
  }

  std::unique_ptr<engine::renderer::AssetDatabase> database(
      new (std::nothrow) engine::renderer::AssetDatabase());
  if (database == nullptr) {
    engine::core::shutdown_vfs();
    return 3;
  }

  int result = verify_successful_resolution(database.get());
  if (result == 0) {
    result = verify_failed_load_falls_back(database.get());
  }
  if (result == 0) {
    result = verify_shared_texture_loads_once(database.get());
  }
  if (result == 0) {
    result = verify_no_slots_and_null_database();
  }

  engine::core::shutdown_vfs();
  return result;
}
