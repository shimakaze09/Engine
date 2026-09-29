// Declares the defaults of the two limits the Lua sandbox enforces: the
// instruction budget every dispatch in a frame shares, and the byte cap of
// the Lua allocator. They are what a VM runs under until something sets
// them, and what a project that names no limits of its own runs under.

#pragma once

#include <cstddef>

namespace engine::scripting {

/// Lua instructions every dispatch, coroutine and hook in one frame share.
inline constexpr int kDefaultInstructionLimit = 1000000;

/// Bytes the Lua allocator may hold.
inline constexpr std::size_t kDefaultMemoryLimit = 64U * 1024U * 1024U;

} // namespace engine::scripting
