// Implements engine-owned storage for an EngineConfig's borrowed strings.

#include "engine_config_strings.h"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "engine/core/logging.h"

namespace engine {

namespace {

/// One fixed buffer per configurable string. engine::bootstrap owns this
/// storage together with the active configuration that points into it: a
/// later bootstrap overwrites it on the same main thread, and nothing
/// resizes, frees, or writes through the pointers handed out.
struct ConfigStringStorage final {
  char assetMount[kMaxConfigStringLength + 1U] = {};
  char assetRoot[kMaxConfigStringLength + 1U] = {};
  char mainScriptPath[kMaxConfigStringLength + 1U] = {};
  char bootstrapMeshPath[kMaxConfigStringLength + 1U] = {};
  char shaderRootPath[kMaxConfigStringLength + 1U] = {};
  char editorScenePath[kMaxConfigStringLength + 1U] = {};
  char editorAssetRoot[kMaxConfigStringLength + 1U] = {};
  char windowTitle[kMaxConfigStringLength + 1U] = {};
};

ConfigStringStorage g_strings{};

/// One string being adopted: `source` is the caller's borrowed pointer,
/// `destination` a buffer in the staged copy, `field` names the
/// configuration member in rejection diagnostics.
struct AdoptionRow final {
  const char *field;
  const char *source;
  char *destination;
};

/// Copies one row into its staged buffer, reporting why it was rejected
/// when it cannot be stored verbatim.
bool stage_string(const AdoptionRow &row) noexcept {
  char message[192] = {};

  if (row.source == nullptr) {
    static_cast<void>(std::snprintf(message, sizeof(message),
                                    "configuration field '%s' is null",
                                    row.field));
    core::log_message(core::LogLevel::Error, "engine", message);
    return false;
  }

  const std::size_t length = std::strlen(row.source);
  if (length > kMaxConfigStringLength) {
    static_cast<void>(std::snprintf(
        message, sizeof(message),
        "configuration field '%s' is %zu characters; the limit is %zu",
        row.field, length, kMaxConfigStringLength));
    core::log_message(core::LogLevel::Error, "engine", message);
    return false;
  }

  std::memcpy(row.destination, row.source, length + 1U);
  return true;
}

} // namespace

/// Copies every borrowed string in `config` into engine-owned storage and
/// repoints `config` at it.
bool adopt_config_strings(EngineConfig &config) noexcept {
  // Staging into a local keeps the commit all-or-nothing: nothing reaches
  // the live storage until every field has been validated, so a rejected
  // configuration cannot leave the active one half-overwritten.
  ConfigStringStorage staged{};

  const std::array<AdoptionRow, 7U> rows{{
      {"assetMount", config.assetMount, staged.assetMount},
      {"assetRoot", config.assetRoot, staged.assetRoot},
      {"mainScriptPath", config.mainScriptPath, staged.mainScriptPath},
      {"bootstrapMeshPath", config.bootstrapMeshPath,
       staged.bootstrapMeshPath},
      {"shaderRootPath", config.shaderRootPath, staged.shaderRootPath},
      {"editorScenePath", config.editorScenePath, staged.editorScenePath},
      {"editorAssetRoot", config.editorAssetRoot, staged.editorAssetRoot},
  }};

  for (const AdoptionRow &row : rows) {
    if (!stage_string(row)) {
      return false;
    }
  }

  // The window title is optional at this layer: core's platform layer
  // substitutes its own default for a null title, so a null passes
  // through unchanged instead of being rejected here or shadowed by a
  // second copy of that default.
  const bool hasTitle = config.core.platform.title != nullptr;
  if (hasTitle && !stage_string({"core.platform.title",
                                 config.core.platform.title,
                                 staged.windowTitle})) {
    return false;
  }

  g_strings = staged;

  config.assetMount = g_strings.assetMount;
  config.assetRoot = g_strings.assetRoot;
  config.mainScriptPath = g_strings.mainScriptPath;
  config.bootstrapMeshPath = g_strings.bootstrapMeshPath;
  config.shaderRootPath = g_strings.shaderRootPath;
  config.editorScenePath = g_strings.editorScenePath;
  config.editorAssetRoot = g_strings.editorAssetRoot;
  if (hasTitle) {
    config.core.platform.title = g_strings.windowTitle;
  }

  return true;
}

} // namespace engine
