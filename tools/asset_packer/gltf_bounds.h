// Declares the bound check every glTF accessor must pass before its count
// is used to size an allocation.
//
// Kept separate from packer_shared.h so the import translation units that
// are unit-tested in isolation can use it without pulling in the cook
// contract and its content-module dependency.

#pragma once

#include <cstddef>

#include <cgltf.h>

/// Largest element count the cooked formats can carry. Mesh and animation
/// headers store counts as uint32, so a larger count could not be written
/// even where it could be allocated.
inline constexpr std::size_t kMaxCookedElementCount = 0xFFFFFFFFULL;

/// True when an accessor's element count may be trusted to size a buffer.
///
/// `cgltf_validate` bounds an accessor against its buffer view only when it
/// has one. An accessor with neither a buffer view nor sparse data is legal
/// glTF — it reads as zeros — and is accepted with any count the JSON
/// number parser produces. Sizing an allocation from that count either
/// wraps, leaving a buffer shorter than the loop that fills it, or asks for
/// terabytes, which terminates the process under the no-exception build.
///
/// `elementsPerItem` is the expansion the caller applies per element (an
/// interleaved float stride, or 1 for a flat copy); the product is checked
/// before any allocation. Refusal prints one line naming `what`; the caller
/// stops without allocating.
bool accessor_count_is_cookable(const cgltf_accessor *accessor,
                                std::size_t elementsPerItem, const char *what);
