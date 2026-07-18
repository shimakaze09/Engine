// Declares allocator types and APIs for the Engine core engine.

#pragma once

#include <cstddef>

namespace engine::core {

using AllocateFunction = void* (*)(void* context, std::size_t sizeBytes, std::size_t alignment) noexcept;
using ResetFunction = void (*)(void* context) noexcept;

/// Type-erased allocation function plus user data (no ownership).
struct Allocator final {
    void* context = nullptr;
    AllocateFunction allocateFunction = nullptr;
    ResetFunction resetFunction = nullptr;

    /// Allocates sizeBytes at the given alignment; nullptr on failure.
    void* allocate_bytes(std::size_t sizeBytes, std::size_t alignment) const noexcept {
        if (allocateFunction == nullptr) {
            return nullptr;
        }

        return allocateFunction(context, sizeBytes, alignment);
    }

    /// Resets this object back to its reusable empty state for all.
    void reset_all() const noexcept {
        if (resetFunction != nullptr) {
            resetFunction(context);
        }
    }
};

} // namespace engine::core
