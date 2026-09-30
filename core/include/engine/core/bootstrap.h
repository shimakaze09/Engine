// Declares bootstrap types and APIs for the Engine core engine.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/asset_identity.h"
#include "engine/core/platform.h"

namespace engine::core {

/// Describes core-owned subsystem startup choices.
struct CoreConfig final {
  bool initializePlatform = true;
  /// Job-system worker threads beside the main thread; 0 picks one per
  /// hardware thread minus the main thread.
  std::uint32_t workerThreads = 0U;
  PlatformConfig platform{};
  /// Content root of the project whose per-user data (the save slot, the
  /// rebound input map) this run reads and writes; see
  /// engine/core/project_data.h. Named before input initializes, which
  /// restores that map. Null names no project, and that data is refused.
  const char *projectRoot = nullptr;
  /// The project's persistent GUID, from its .project document. When valid
  /// it names the project instead of projectRoot, so the data survives
  /// renaming or moving the project directory.
  AssetGuid projectGuid{};
};

/// Initializes the owning system for core with explicit subsystem ownership.
bool initialize_core(const CoreConfig &config) noexcept;
/// Shuts down the owning system for core.
void shutdown_core() noexcept;
/// Returns whether is core initialized.
bool is_core_initialized() noexcept;

}  // namespace engine::core
