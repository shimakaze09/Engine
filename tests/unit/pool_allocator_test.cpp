// Verifies pool allocator test behavior for the Engine test suite.

#include "engine/core/pool_allocator.h"

#include <cstddef>
#include <cstdint>

namespace {

/// Stores pointer sized item data used by the engine.
struct PointerSizedItem final {
  void *data = nullptr;
};

/// Stores two pointer item data used by the engine.
struct TwoPointerItem final {
  void *a = nullptr;
  void *b = nullptr;
};

struct CountingItem final {
  static int constructed;
  static int destroyed;

  int value = 0;

  CountingItem() noexcept { ++constructed; }
  ~CountingItem() noexcept { ++destroyed; }
};

int CountingItem::constructed = 0;
int CountingItem::destroyed = 0;

} // namespace

/// Runs this executable or test program.
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
    if (p->data != nullptr) {
      return 9;
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

  // Test 5: constructors/destructors are paired.
  {
    CountingItem::constructed = 0;
    CountingItem::destroyed = 0;

    {
      engine::core::PoolAllocator<CountingItem, 2U> pool;
      CountingItem *a = pool.allocate();
      if ((a == nullptr) || (CountingItem::constructed != 1) ||
          (CountingItem::destroyed != 0)) {
        return 10;
      }
      a->value = 42;
      pool.deallocate(a);
      if ((CountingItem::constructed != 1) ||
          (CountingItem::destroyed != 1)) {
        return 11;
      }
      CountingItem *b = pool.allocate();
      if ((b == nullptr) || (CountingItem::constructed != 2) ||
          (CountingItem::destroyed != 1)) {
        return 12;
      }
      b->value = 7;
    }

    if ((CountingItem::constructed != 2) || (CountingItem::destroyed != 2)) {
      return 13;
    }
  }

  return 0;
}
