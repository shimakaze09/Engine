// Declares a fixed-count, move-only, nothrow-allocating buffer shared by
// runtime and renderer asset loaders.

#pragma once

#include <cstddef>
#include <initializer_list>
#include <limits>
#include <memory>
#include <new>

namespace engine::core {

/// Move-only owned buffer whose element count is bound to its allocation:
/// allocate/assign are the only growth paths and report failure instead of
/// terminating, so recoverable out-of-memory keeps normal load-failure
/// semantics under the no-exception build while the buffer stays the single
/// source of allocation truth (originally the renderer's mesh_loader.h
/// MeshBuffer<T>, audit H-11; generalized to core so runtime's animation
/// payload loader can share it, audit #174).
template <typename T>
class NothrowBuffer final {
 public:
  NothrowBuffer() = default;
  NothrowBuffer(NothrowBuffer &&) noexcept = default;
  NothrowBuffer &operator=(NothrowBuffer &&) noexcept = default;
  NothrowBuffer(const NothrowBuffer &) = delete;
  NothrowBuffer &operator=(const NothrowBuffer &) = delete;
  ~NothrowBuffer() = default;

  /// Replaces the contents with count zero-initialized elements; false and
  /// empty on allocation failure.
  [[nodiscard]] bool allocate(std::size_t count) noexcept {
    m_data.reset();
    m_count = 0U;
    if (count == 0U) {
      return true;
    }
    if (count > (std::numeric_limits<std::size_t>::max() / sizeof(T))) {
      return false;
    }
    m_data.reset(new (std::nothrow) T[count]{});
    if (m_data == nullptr) {
      return false;
    }
    m_count = count;
    return true;
  }

  /// Replaces the contents with a copy of values; false and empty on
  /// allocation failure.
  [[nodiscard]] bool assign(std::initializer_list<T> values) noexcept {
    return assign(values.begin(), values.size());
  }

  /// Replaces the contents with a copy of count elements starting at
  /// srcData; false and empty on allocation failure or a null source with
  /// a nonzero count.
  [[nodiscard]] bool assign(const T *srcData, std::size_t count) noexcept {
    if ((srcData == nullptr) && (count != 0U)) {
      return false;
    }
    if (!allocate(count)) {
      return false;
    }
    for (std::size_t index = 0U; index < count; ++index) {
      m_data[index] = srcData[index];
    }
    return true;
  }

  void clear() noexcept {
    m_data.reset();
    m_count = 0U;
  }

  std::size_t size() const noexcept { return m_count; }
  bool empty() const noexcept { return m_count == 0U; }
  T *data() noexcept { return m_data.get(); }
  const T *data() const noexcept { return m_data.get(); }
  T &operator[](std::size_t index) noexcept { return m_data[index]; }
  const T &operator[](std::size_t index) const noexcept {
    return m_data[index];
  }

 private:
  std::unique_ptr<T[]> m_data{};
  std::size_t m_count = 0U;
};

} // namespace engine::core
