// Declares engine-owned storage for an EngineConfig's borrowed strings.

#pragma once

#include <cstddef>

#include "engine/engine.h"

namespace engine {

/// Longest configuration string the engine stores, matching core's VFS
/// OS-path bound so an absolute project root fits. A longer value is
/// rejected rather than truncated: a truncated path names a different
/// file, and silently reading the wrong one is worse than not booting.
inline constexpr std::size_t kMaxConfigStringLength = 259U;

/// Copies every borrowed string in `config` into engine-owned storage of
/// static lifetime and repoints `config` at that storage, so runtime and
/// editor systems reading them frames later cannot outlive the caller's
/// buffers. All-or-nothing: a rejected string (a null path, or one longer
/// than kMaxConfigStringLength) leaves both the storage and `config`
/// untouched and returns false with a diagnostic naming the field, so a
/// previously adopted configuration stays valid. A null window title
/// stays null — core's platform layer owns that field's fallback.
bool adopt_config_strings(EngineConfig &config) noexcept;

} // namespace engine
