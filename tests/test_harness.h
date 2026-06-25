// Declares tiny shared helpers for unit test assertions.

#pragma once

#include <cstdio>

namespace engine::tests {

/// Tracks pass/fail counts for small standalone test executables.
class TestContext final {
public:
  /// Records a named assertion result.
  void check(bool condition, const char *name) noexcept {
    if (condition) {
      ++m_passed;
      return;
    }

    ++m_failed;
    std::fprintf(stderr, "FAIL: %s\n", name);
  }

  /// Records a named unconditional failure.
  void fail(const char *name) noexcept { check(false, name); }

  /// Prints a suite summary and returns a process exit code.
  int finish(const char *suiteName) const noexcept {
    std::fprintf(stdout, "%s: %d passed, %d failed\n", suiteName, m_passed,
                 m_failed);
    return (m_failed == 0) ? 0 : 1;
  }

  /// Returns the number of passed assertions.
  int passed() const noexcept { return m_passed; }
  /// Returns the number of failed assertions.
  int failed() const noexcept { return m_failed; }

private:
  int m_passed = 0;
  int m_failed = 0;
};

} // namespace engine::tests
