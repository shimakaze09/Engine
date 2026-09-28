// Declares the one way a test finds the bundled assets: the directory,
// at or up to four levels above the working directory, that holds the
// game's assets/main.lua and the engine content's shader manifest
// (engine_assets/, mounted at engine/). Tests run from their build
// directory, whose parents hold the build's copies of both and the source
// tree's; either answers.

#pragma once

#include <filesystem>
#include <string>
#include <system_error>

namespace engine::tests {

/// Finds the directory holding the bundled assets/ and engine_assets/ and
/// writes it to `*out`; false, `*out` untouched, when no candidate holds
/// them.
inline bool find_asset_root(std::filesystem::path *out) noexcept {
  std::error_code ec{};
  const std::filesystem::path original = std::filesystem::current_path(ec);
  if (ec || (out == nullptr)) {
    return false;
  }
  std::filesystem::path candidate = original;
  for (int depth = 0; depth < 5; ++depth) {
    ec.clear();
    const std::filesystem::path normalized =
        std::filesystem::weakly_canonical(candidate, ec);
    if (!ec && std::filesystem::exists(normalized / "assets/main.lua", ec) &&
        std::filesystem::exists(
            normalized / "engine_assets/shaders/bgfx/shaders.manifest", ec)) {
      *out = normalized;
      return true;
    }
    candidate /= "..";
  }
  return false;
}

/// Makes the directory find_asset_root finds the working directory, so the
/// engine's relative "assets" paths resolve; false when it is not found or
/// cannot be entered.
inline bool enter_asset_root() noexcept {
  std::filesystem::path root;
  if (!find_asset_root(&root)) {
    return false;
  }
  std::error_code ec{};
  std::filesystem::current_path(root, ec);
  return !ec;
}

/// The engine content directory (engine_assets/) beside the bundled
/// assets, as an absolute path, for a test that bootstraps from a scratch
/// working directory and so must name EngineConfig::engineRoot itself;
/// empty when the assets cannot be found, which bootstrap then refuses.
inline std::string engine_root_path() {
  std::filesystem::path root;
  if (!find_asset_root(&root)) {
    return std::string();
  }
  return (root / "engine_assets").string();
}

} // namespace engine::tests
