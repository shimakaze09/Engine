// Declares the one way a test finds the bundled assets: the directory,
// at or up to four levels above the working directory, that holds
// assets/main.lua and the cooked-shader manifest. Tests run from their
// build directory, whose parents hold the build's copy of assets/ and the
// source tree's; either answers.

#pragma once

#include <filesystem>
#include <system_error>

namespace engine::tests {

/// Finds the directory holding the bundled assets/ and writes it to
/// `*out`; false, `*out` untouched, when no candidate holds them.
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
            normalized / "assets/shaders/bgfx/shaders.manifest", ec)) {
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

} // namespace engine::tests
