// Declares the one way a test finds the bundled content: the sample
// project (samples/island, holding island.project and its assets/) and
// the engine's own content (engine_assets/), found together in the
// directory at or up to four levels above the working directory. Tests
// run from their build directory, whose parents hold the build's copies
// of both and the source tree's; either answers. Entering the sample
// project makes it the working directory, so the engine's relative
// "assets" paths name its content, and points ENGINE_ROOT at the engine
// content, which the engine looks up there when a config leaves
// EngineConfig::engineRoot empty.

#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

namespace engine::tests {

/// The sample project, relative to the directory holding it and
/// engine_assets/.
inline constexpr const char *kSampleProjectDirectory = "samples/island";

/// Finds the directory holding samples/island/ and engine_assets/ and
/// writes it to `*out`; false, `*out` untouched, when no candidate holds
/// them.
inline bool find_workspace_root(std::filesystem::path *out) noexcept {
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
    if (!ec &&
        std::filesystem::exists(
            normalized / kSampleProjectDirectory / "island.project", ec) &&
        std::filesystem::exists(
            normalized / "engine_assets/shaders/bgfx/shaders.manifest", ec)) {
      *out = normalized;
      return true;
    }
    candidate /= "..";
  }
  return false;
}

/// The sample project's directory, absolute; empty when it cannot be
/// found.
inline std::string sample_project_path() {
  std::filesystem::path root;
  if (!find_workspace_root(&root)) {
    return std::string();
  }
  return (root / kSampleProjectDirectory).string();
}

/// The engine content directory (engine_assets/), absolute, for a test
/// that names EngineConfig::engineRoot itself; empty when it cannot be
/// found, which bootstrap then refuses.
inline std::string engine_root_path() {
  std::filesystem::path root;
  if (!find_workspace_root(&root)) {
    return std::string();
  }
  return (root / "engine_assets").string();
}

/// Makes the sample project the working directory and points ENGINE_ROOT
/// at the engine content, so a default EngineConfig bootstraps the
/// sample; false when either cannot be found or entered.
inline bool enter_asset_root() noexcept {
  std::filesystem::path root;
  if (!find_workspace_root(&root)) {
    return false;
  }
  const std::string engineRoot = (root / "engine_assets").string();
#if defined(_WIN32)
  if (_putenv_s("ENGINE_ROOT", engineRoot.c_str()) != 0) {
    return false;
  }
#else
  if (setenv("ENGINE_ROOT", engineRoot.c_str(), 1) != 0) {
    return false;
  }
#endif
  std::error_code ec{};
  std::filesystem::current_path(root / kSampleProjectDirectory, ec);
  return !ec;
}

} // namespace engine::tests
