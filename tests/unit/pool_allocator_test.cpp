#include "engine/core/pool_allocator.h"

#include <cstddef>
#include <cstdint>

namespace {

struct PointerSizedItem final {
  void *data = nullptr;
};

struct TwoPointerItem final {
  void *a = nullptr;
  void *b = nullptr;
};

} // namespace

int main() {
  // Test 1: Basic alloc/dealloc
  {
    engine::core::PoolAllocator<PointerSizedItem, 4U> pool;

    if (pool.capacity() != 4U) {
      return 1;
    }

    PointerSizedItem *p = pool.allocate();
    if (p == nullptr) {
      return 2;
    }

    p->data = reinterpret_cast<void *>(0xDEADBEEFULL);
    pool.deallocate(p);

    // After dealloc, should be able to alloc again.
    PointerSizedItem *p2 = pool.allocate();
    if (p2 == nullptr) {
      return 3;
    }
    pool.deallocate(p2);
  }

  // Test 2: Alloc until exhausted returns nullptr.
  {
    engine::core::PoolAllocator<TwoPointerItem, 3U> pool;

    TwoPointerItem *a = pool.allocate();
    TwoPointerItem *b = pool.allocate();
    TwoPointerItem *c = pool.allocate();
    TwoPointerItem *d = pool.allocate(); // should be nullptr

    if (a == nullptr || b == nullptr || c == nullptr) {
      return 4;
    }
    if (d != nullptr) {
      return 5;
    }

    pool.deallocate(a);
    pool.deallocate(b);
    pool.deallocate(c);
  }

  // Test 3: Dealloc then realloc returns a valid slot.
  {
    engine::core::PoolAllocator<PointerSizedItem, 2U> pool;

    PointerSizedItem *x = pool.allocate();
    PointerSizedItem *y = pool.allocate();
    if (x == nullptr || y == nullptr) {
      return 6;
    }

    // Pool exhausted.
    if (pool.allocate() != nullptr) {
      return 7;
    }

    // Dealloc one, then realloc.
    pool.deallocate(x);
    PointerSizedItem *z = pool.allocate();
    if (z == nullptr) {
      return 8;
    }

    pool.deallocate(y);
    pool.deallocate(z);
  }

  // Test 4: nullptr dealloc is safe.
  {
    engine::core::PoolAllocator<PointerSizedItem, 2U> pool;
    pool.deallocate(nullptr); // must not crash
  }

  return 0;
}
