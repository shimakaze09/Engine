#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "engine/core/bootstrap.h"
#include "engine/core/job_system.h"
#include "engine/core/linear_allocator.h"
#include "engine/math/vec4.h"

namespace {

struct WriteOrderJobData final {
  std::atomic<std::int32_t> *cursor = nullptr;
  std::array<std::int32_t, 4U> *order = nullptr;
  std::int32_t value = 0;
};

void write_order_job(void *userData) noexcept {
  auto *data = static_cast<WriteOrderJobData *>(userData);
  if ((data == nullptr) || (data->cursor == nullptr) ||
      (data->order == nullptr)) {
    return;
  }

  const std::int32_t slot =
      data->cursor->fetch_add(1, std::memory_order_acq_rel);
  if ((slot >= 0) && (slot < static_cast<std::int32_t>(data->order->size()))) {
    (*data->order)[static_cast<std::size_t>(slot)] = data->value;
  }
}

void noop_job(void *) noexcept {}

} // namespace

int main() {
  constexpr bool kVec4LayoutOk = (alignof(engine::math::Vec4) == 16U) &&
                                 (sizeof(engine::math::Vec4) == 16U);
  if constexpr (!kVec4LayoutOk) {
    return 1;
  }

  const engine::math::Vec4 a(1.0F, 2.0F, 3.0F, 4.0F);
  const engine::math::Vec4 b(4.0F, 3.0F, 2.0F, 1.0F);
  const engine::math::Vec4 c = engine::math::add(a, b);
  const engine::math::Vec4 d = engine::math::mul(a, 0.5F);

  if ((c.x != 5.0F) || (c.y != 5.0F) || (c.z != 5.0F) || (c.w != 5.0F)) {
    return 2;
  }

  if ((d.x != 0.5F) || (d.y != 1.0F) || (d.z != 1.5F) || (d.w != 2.0F)) {
    return 3;
  }

  if (engine::math::dot(a, b) != 20.0F) {
    return 4;
  }

  std::array<std::byte, 256U> memory{};
  engine::core::LinearAllocator allocator;
  allocator.init(memory.data(), memory.size());

  void *p0 = allocator.allocate(32U, 16U);
  void *p1 = allocator.allocate(32U, 16U);
  void *p2 = allocator.allocate(512U, 16U);
  void *pOverflow =
      allocator.allocate(std::numeric_limits<std::size_t>::max(), 16U);

  if ((p0 == nullptr) || (p1 == nullptr)) {
    return 5;
  }

  if (p2 != nullptr) {
    return 6;
  }

  if (pOverflow != nullptr) {
    return 25;
  }

  if ((reinterpret_cast<std::uintptr_t>(p0) & 15U) != 0U) {
    return 7;
  }

  if ((reinterpret_cast<std::uintptr_t>(p1) & 15U) != 0U) {
    return 8;
  }

  if (allocator.allocation_count() != 2U) {
    return 9;
  }

  const auto allocatorInterface = engine::core::make_allocator(&allocator);
  void *p3 = allocatorInterface.allocate_bytes(8U, 8U);

  if (p3 == nullptr) {
    return 10;
  }

  allocatorInterface.reset_all();

  if ((allocator.bytes_used() != 0U) || (allocator.allocation_count() != 0U)) {
    return 11;
  }

  if (!engine::core::initialize_core(1024U * 1024U)) {
    return 12;
  }

  if (!engine::core::begin_frame_graph()) {
    engine::core::shutdown_core();
    return 13;
  }

  std::atomic<std::int32_t> cursor = 0;
  std::array<std::int32_t, 4U> order = {0, 0, 0, 0};

  WriteOrderJobData first{};
  first.cursor = &cursor;
  first.order = &order;
  first.value = 1;

  WriteOrderJobData second{};
  second.cursor = &cursor;
  second.order = &order;
  second.value = 2;

  WriteOrderJobData third{};
  third.cursor = &cursor;
  third.order = &order;
  third.value = 3;

  engine::core::Job firstJob{};
  firstJob.function = &write_order_job;
  firstJob.data = &first;
  const engine::core::JobHandle firstHandle = engine::core::submit(firstJob);

  engine::core::Job secondJob{};
  secondJob.function = &write_order_job;
  secondJob.data = &second;
  const engine::core::JobHandle secondHandle = engine::core::submit(secondJob);

  engine::core::Job thirdJob{};
  thirdJob.function = &write_order_job;
  thirdJob.data = &third;
  const engine::core::JobHandle thirdHandle = engine::core::submit(thirdJob);

  if (!engine::core::is_valid_handle(firstHandle) ||
      !engine::core::is_valid_handle(secondHandle) ||
      !engine::core::is_valid_handle(thirdHandle)) {
    static_cast<void>(engine::core::end_frame_graph());
    engine::core::shutdown_core();
    return 14;
  }

  if (!engine::core::add_dependency(firstHandle, thirdHandle) ||
      !engine::core::add_dependency(secondHandle, thirdHandle)) {
    static_cast<void>(engine::core::end_frame_graph());
    engine::core::shutdown_core();
    return 15;
  }

  engine::core::wait(thirdHandle);

  if (!engine::core::end_frame_graph()) {
    engine::core::shutdown_core();
    return 16;
  }

  if (!engine::core::begin_frame_graph()) {
    engine::core::shutdown_core();
    return 17;
  }

  engine::core::Job cycleJobA{};
  cycleJobA.function = &noop_job;
  cycleJobA.data = nullptr;
  const engine::core::JobHandle cycleHandleA = engine::core::submit(cycleJobA);

  engine::core::Job cycleJobB{};
  cycleJobB.function = &noop_job;
  cycleJobB.data = nullptr;
  const engine::core::JobHandle cycleHandleB = engine::core::submit(cycleJobB);

  if (!engine::core::is_valid_handle(cycleHandleA) ||
      !engine::core::is_valid_handle(cycleHandleB)) {
    static_cast<void>(engine::core::end_frame_graph());
    engine::core::shutdown_core();
    return 18;
  }

  if (!engine::core::add_dependency(cycleHandleA, cycleHandleB)) {
    static_cast<void>(engine::core::end_frame_graph());
    engine::core::shutdown_core();
    return 19;
  }

  if (engine::core::add_dependency(cycleHandleB, cycleHandleA)) {
    static_cast<void>(engine::core::end_frame_graph());
    engine::core::shutdown_core();
    return 20;
  }

  engine::core::wait(cycleHandleB);

  if (!engine::core::end_frame_graph()) {
    engine::core::shutdown_core();
    return 21;
  }

  engine::core::shutdown_core();

  if (cursor.load(std::memory_order_acquire) != 3) {
    return 22;
  }

  if (order[2] != 3) {
    return 23;
  }

  return (allocator.capacity() == 256U) ? 0 : 24;
}
