#pragma once

#include <cstdio>

namespace engine::tests {

class TestContext final {
public:
  void check(bool condition, const char *name) noexcept {
    if (condition) {
      ++m_passed;
      return;
    }

    ++m_failed;
    std::fprintf(stderr, "FAIL: %s\n", name);
  }

  void fail(const char *name) noexcept { check(false, name); }

  int finish(const char *suiteName) const noexcept {
    std::fprintf(stdout, "%s: %d passed, %d failed\n", suiteName, m_passed,
                 m_failed);
    return (m_failed == 0) ? 0 : 1;
  }

  int passed() const noexcept { return m_passed; }
  int failed() const noexcept { return m_failed; }

private:
  int m_passed = 0;
  int m_failed = 0;
};

} // namespace engine::tests
