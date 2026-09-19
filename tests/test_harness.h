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

  /// Records a named check that could not run in this environment. It is
  /// neither a pass nor a failure: the suite still exits 0, and the
  /// SKIPPED: line lets a ctest registration carrying
  /// SKIP_REGULAR_EXPRESSION "SKIPPED:" report the run as skipped instead
  /// of counting absent coverage as a pass.
  void skip(const char *name) noexcept {
    ++m_skipped;
    std::fprintf(stdout, "SKIPPED: %s\n", name);
  }

  /// Prints a suite summary and returns a process exit code.
  int finish(const char *suiteName) const noexcept {
    std::fprintf(stdout, "%s: %d passed, %d failed", suiteName, m_passed,
                 m_failed);
    if (m_skipped > 0) {
      std::fprintf(stdout, ", %d skipped", m_skipped);
    }
    std::fprintf(stdout, "\n");
    return (m_failed == 0) ? 0 : 1;
  }

  /// Returns the number of passed assertions.
  int passed() const noexcept { return m_passed; }
  /// Returns the number of failed assertions.
  int failed() const noexcept { return m_failed; }
  /// Returns the number of checks recorded as skipped.
  int skipped() const noexcept { return m_skipped; }

private:
  int m_passed = 0;
  int m_failed = 0;
  int m_skipped = 0;
};

} // namespace engine::tests
