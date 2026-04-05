#include "engine/core/linear_allocator.h"

#include <cstdint>
#include <limits>

namespace engine::core {

namespace {

bool is_power_of_two(std::size_t value) noexcept {
  return (value != 0U) && ((value & (value - 1U)) == 0U);
}

void *allocator_allocate(void *context, std::size_t sizeBytes,
                         std::size_t alignment) noexcept {
  if (context == nullptr) {
    return nullptr;
  }

  auto *allocator = static_cast<LinearAllocator *>(context);
  return allocator->allocate(sizeBytes, alignment);
}

void allocator_reset(void *context) noexcept {
  if (context != nullptr) {
    auto *allocator = static_cast<LinearAllocator *>(context);
    allocator->reset();
  }
}

} // namespace

void LinearAllocator::init(void *memory, std::size_t capacityBytes) noexcept {
  m_memory = static_cast<std::byte *>(memory);
  m_capacity = capacityBytes;
  m_head = 0;
  m_allocationCount = 0;
}

void *LinearAllocator::allocate(std::size_t sizeBytes,
                                std::size_t alignment) noexcept {
  if ((m_memory == nullptr) || (sizeBytes == 0U)) {
    return nullptr;
  }

  if ((alignment == 0U) || !is_power_of_two(alignment)) {
    return nullptr;
  }

  if (m_head > m_capacity) {
    return nullptr;
  }

  if (m_head >
      static_cast<std::size_t>(std::numeric_limits<std::uintptr_t>::max())) {
    return nullptr;
  }

  const auto baseAddress = reinterpret_cast<std::uintptr_t>(m_memory);
  const auto headAddressOffset = static_cast<std::uintptr_t>(m_head);
  if (headAddressOffset >
      (std::numeric_limits<std::uintptr_t>::max() - baseAddress)) {
    return nullptr;
  }

  const auto currentAddress = baseAddress + headAddressOffset;
  const auto alignmentMask = static_cast<std::uintptr_t>(alignment - 1U);
  if (currentAddress >
      (std::numeric_limits<std::uintptr_t>::max() - alignmentMask)) {
    return nullptr;
  }

  const auto alignedAddress = (currentAddress + alignmentMask) & ~alignmentMask;
  const auto padding =
      static_cast<std::size_t>(alignedAddress - currentAddress);
  const std::size_t remaining = m_capacity - m_head;

  if ((padding > remaining) || (sizeBytes > (remaining - padding))) {
    return nullptr;
  }

  m_head += padding;
  m_head += sizeBytes;
  ++m_allocationCount;
  return reinterpret_cast<void *>(alignedAddress);
}

void LinearAllocator::reset() noexcept {
  m_head = 0;
  m_allocationCount = 0;
}

std::size_t LinearAllocator::capacity() const noexcept { return m_capacity; }

std::size_t LinearAllocator::bytes_used() const noexcept { return m_head; }

std::size_t LinearAllocator::allocation_count() const noexcept {
  return m_allocationCount;
}

Allocator make_allocator(LinearAllocator *allocator) noexcept {
  Allocator interfaceAllocator{};
  interfaceAllocator.context = allocator;
  interfaceAllocator.allocateFunction = &allocator_allocate;
  interfaceAllocator.resetFunction = &allocator_reset;
  return interfaceAllocator;
}

} // namespace engine::core
