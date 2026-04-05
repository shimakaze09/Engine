#pragma once

#include <cstddef>

namespace engine::core {

using AllocateFunction = void* (*)(void* context, std::size_t sizeBytes, std::size_t alignment) noexcept;
using ResetFunction = void (*)(void* context) noexcept;

struct Allocator final {
    void* context = nullptr;
    AllocateFunction allocateFunction = nullptr;
    ResetFunction resetFunction = nullptr;

    void* allocate_bytes(std::size_t sizeBytes, std::size_t alignment) const noexcept {
        if (allocateFunction == nullptr) {
            return nullptr;
        }

        return allocateFunction(context, sizeBytes, alignment);
    }

    void reset_all() const noexcept {
        if (resetFunction != nullptr) {
            resetFunction(context);
        }
    }
};

} // namespace engine::core
