// Verifies cvar console test behavior for the Engine test suite.

#include <cstdio>
#include <cstring>
#include <atomic>
#include <thread>

#include "engine/core/console.h"
#include "engine/core/cvar.h"

using namespace engine::core;

// ---- CVar tests ----

static bool test_cvar_register_and_get() noexcept {
  initialize_cvars();

  if (!cvar_register_bool("test.bool", true, "a bool cvar")) {
    return false;
  }
  if (!cvar_get_bool("test.bool", false)) {
    return false;
  }

  if (!cvar_register_int("test.int", 42, "an int cvar")) {
    return false;
  }
  if (cvar_get_int("test.int", 0) != 42) {
    return false;
  }

  if (!cvar_register_float("test.float", 3.14F, "a float cvar")) {
    return false;
  }
  if (cvar_get_float("test.float", 0.0F) < 3.13F) {
    return false;
  }

  if (!cvar_register_string("test.str", "hello", "a string cvar")) {
    return false;
  }
  if (std::strcmp(cvar_get_string("test.str", ""), "hello") != 0) {
    return false;
  }

  shutdown_cvars();
  return true;
}

static bool test_cvar_set() noexcept {
  initialize_cvars();

  cvar_register_bool("cv.b", false, "d");
  cvar_register_int("cv.i", 0, "d");
  cvar_register_float("cv.f", 0.0F, "d");
  cvar_register_string("cv.s", "a", "d");

  if (!cvar_set_bool("cv.b", true)) {
    shutdown_cvars();
    return false;
  }
  if (!cvar_get_bool("cv.b", false)) {
    shutdown_cvars();
    return false;
  }

  if (!cvar_set_int("cv.i", 99)) {
    shutdown_cvars();
    return false;
  }
  if (cvar_get_int("cv.i", 0) != 99) {
    shutdown_cvars();
    return false;
  }

  if (!cvar_set_float("cv.f", 1.5F)) {
    shutdown_cvars();
    return false;
  }
  if (cvar_get_float("cv.f", 0.0F) < 1.49F) {
    shutdown_cvars();
    return false;
  }

  if (!cvar_set_string("cv.s", "world")) {
    shutdown_cvars();
    return false;
  }
  if (std::strcmp(cvar_get_string("cv.s", ""), "world") != 0) {
    shutdown_cvars();
    return false;
  }

  shutdown_cvars();
  return true;
}

static bool test_cvar_set_from_string() noexcept {
  initialize_cvars();

  cvar_register_bool("p.b", false, "d");
  cvar_register_int("p.i", 0, "d");
  cvar_register_float("p.f", 0.0F, "d");

  if (!cvar_set_from_string("p.b", "true")) {
    shutdown_cvars();
    return false;
  }
  if (!cvar_get_bool("p.b", false)) {
    shutdown_cvars();
    return false;
  }

  if (!cvar_set_from_string("p.i", "77")) {
    shutdown_cvars();
    return false;
  }
  if (cvar_get_int("p.i", 0) != 77) {
    shutdown_cvars();
    return false;
  }

  if (!cvar_set_from_string("p.f", "2.5")) {
    shutdown_cvars();
    return false;
  }
  if (cvar_get_float("p.f", 0.0F) < 2.4F) {
    shutdown_cvars();
    return false;
  }

  shutdown_cvars();
  return true;
}

/// Malformed textual values must be rejected whole with the stored value
/// unchanged: no partial application, trailing garbage, overflow,
/// non-finite floats, loose booleans, or silent string truncation
/// (audit M-10).
static bool test_cvar_set_from_string_rejects_malformed() noexcept {
  initialize_cvars();

  cvar_register_bool("m.b", true, "d");
  cvar_register_int("m.i", 42, "d");
  cvar_register_float("m.f", 1.5F, "d");
  cvar_register_string("m.s", "keep", "d");

  const char *badBools[] = {"banana", "yes", "TRUE", "2", ""};
  for (const char *bad : badBools) {
    if (cvar_set_from_string("m.b", bad) || !cvar_get_bool("m.b", false)) {
      shutdown_cvars();
      return false;
    }
  }
  if (!cvar_set_from_string("m.b", "0") || cvar_get_bool("m.b", true)) {
    shutdown_cvars();
    return false;
  }
  if (!cvar_set_from_string("m.b", "1") || !cvar_get_bool("m.b", false)) {
    shutdown_cvars();
    return false;
  }

  const char *badInts[] = {"12abc", "", " 7", "7 ", "99999999999",
                           "-99999999999", "1.5"};
  for (const char *bad : badInts) {
    if (cvar_set_from_string("m.i", bad) || (cvar_get_int("m.i", 0) != 42)) {
      shutdown_cvars();
      return false;
    }
  }
  if (!cvar_set_from_string("m.i", "-5") || (cvar_get_int("m.i", 0) != -5)) {
    shutdown_cvars();
    return false;
  }

  const char *badFloats[] = {"1.5x", "", "inf", "-inf", "nan", "1e40"};
  for (const char *bad : badFloats) {
    if (cvar_set_from_string("m.f", bad) ||
        (cvar_get_float("m.f", 0.0F) != 1.5F)) {
      shutdown_cvars();
      return false;
    }
  }
  if (!cvar_set_from_string("m.f", "-0.25") ||
      (cvar_get_float("m.f", 0.0F) != -0.25F)) {
    shutdown_cvars();
    return false;
  }

  char longValue[80] = {};
  for (std::size_t i = 0U; i < 64U; ++i) {
    longValue[i] = 'x';
  }
  if (cvar_set_from_string("m.s", longValue) ||
      (std::strcmp(cvar_get_string("m.s", ""), "keep") != 0)) {
    shutdown_cvars();
    return false;
  }
  longValue[63] = '\0';
  if (!cvar_set_from_string("m.s", longValue) ||
      (std::strcmp(cvar_get_string("m.s", ""), longValue) != 0)) {
    shutdown_cvars();
    return false;
  }

  if (cvar_set_from_string("m.unknown", "1")) {
    shutdown_cvars();
    return false;
  }

  shutdown_cvars();
  return true;
}

static bool test_cvar_duplicate_rejected() noexcept {
  initialize_cvars();
  cvar_register_int("dup", 1, "d");
  if (cvar_register_int("dup", 2, "d")) {
    shutdown_cvars();
    return false;
  }
  if (cvar_get_int("dup", 0) != 1) {
    shutdown_cvars();
    return false;
  }
  shutdown_cvars();
  return true;
}

static bool test_cvar_enumerate() noexcept {
  initialize_cvars();
  cvar_register_bool("enum.a", true, "d");
  cvar_register_int("enum.b", 5, "d");
  cvar_register_float("enum.c", 1.0F, "d");

  CVarInfo infos[8] = {};
  const std::size_t count = cvar_get_all(infos, 8U);
  if (count != 3U) {
    shutdown_cvars();
    return false;
  }

  shutdown_cvars();
  return true;
}

static bool test_cvar_null_names_after_registration() noexcept {
  initialize_cvars();
  cvar_register_bool("null.b", true, "d");
  cvar_register_int("null.i", 5, "d");
  cvar_register_float("null.f", 1.0F, "d");
  cvar_register_string("null.s", "value", "d");

  if (!cvar_get_bool(nullptr, true)) {
    shutdown_cvars();
    return false;
  }
  if (cvar_get_int(nullptr, 17) != 17) {
    shutdown_cvars();
    return false;
  }
  if (cvar_get_float(nullptr, 2.0F) != 2.0F) {
    shutdown_cvars();
    return false;
  }
  if (std::strcmp(cvar_get_string(nullptr, "fallback"), "fallback") != 0) {
    shutdown_cvars();
    return false;
  }
  if (cvar_set_bool(nullptr, false) || cvar_set_int(nullptr, 9) ||
      cvar_set_float(nullptr, 3.0F) ||
      cvar_set_string(nullptr, "new") ||
      cvar_set_from_string(nullptr, "new")) {
    shutdown_cvars();
    return false;
  }

  shutdown_cvars();
  return true;
}

// ---- Console tests ----

static bool test_console_basic_execute() noexcept {
  initialize_cvars();
  initialize_console();

  // "help" is registered as a built-in
  if (!console_execute("help")) {
    shutdown_console();
    shutdown_cvars();
    return false;
  }
  if (console_output_line_count() == 0U) {
    shutdown_console();
    shutdown_cvars();
    return false;
  }

  shutdown_console();
  shutdown_cvars();
  return true;
}

static bool test_console_unknown_command() noexcept {
  initialize_cvars();
  initialize_console();

  // Unknown command should return false
  if (console_execute("nonexistent_command_xyz")) {
    shutdown_console();
    shutdown_cvars();
    return false;
  }

  shutdown_console();
  shutdown_cvars();
  return true;
}

static bool test_console_set_get_cvar() noexcept {
  initialize_cvars();
  cvar_register_int("console.test.i", 0, "console set/get test");
  initialize_console();

  if (!console_execute("set console.test.i 123")) {
    shutdown_console();
    shutdown_cvars();
    return false;
  }
  if (cvar_get_int("console.test.i", 0) != 123) {
    shutdown_console();
    shutdown_cvars();
    return false;
  }

  if (!console_execute("get console.test.i")) {
    shutdown_console();
    shutdown_cvars();
    return false;
  }

  shutdown_console();
  shutdown_cvars();
  return true;
}

static bool test_console_custom_command() noexcept {
  initialize_cvars();
  initialize_console();

  static bool s_called = false;
  static int s_argCount = 0;

  auto cmd = [](const char *const * /*args*/, int argCount,
                void * /*ud*/) noexcept {
    s_called = true;
    s_argCount = argCount;
  };

  if (!console_register_command("mytest", cmd, nullptr, "test cmd")) {
    shutdown_console();
    shutdown_cvars();
    return false;
  }
  if (!console_execute("mytest argA argB")) {
    shutdown_console();
    shutdown_cvars();
    return false;
  }
  if (!s_called || s_argCount != 3) {
    shutdown_console();
    shutdown_cvars();
    return false;
  }

  shutdown_console();
  shutdown_cvars();
  return true;
}

static bool test_console_output_ring_buffer() noexcept {
  initialize_cvars();
  initialize_console();

  console_print("line one");
  console_print("line two");

  if (console_output_line_count() < 2U) {
    shutdown_console();
    shutdown_cvars();
    return false;
  }

  char buf[256] = {};
  if (!console_get_output_line(0U, buf, sizeof(buf))) {
    shutdown_console();
    shutdown_cvars();
    return false;
  }

  shutdown_console();
  shutdown_cvars();
  return true;
}

// ---- handle access ----

/// A handle reads what the name reads, for every type, and sees a later
/// set by name (live tuning).
static bool test_cvar_handle_reads_match_name() noexcept {
  initialize_cvars();
  cvar_register_bool("h.b", true, "d");
  cvar_register_int("h.i", 7, "d");
  cvar_register_float("h.f", 2.5F, "d");
  cvar_register_string("h.s", "abc", "d");

  const CVarHandle hb = cvar_find("h.b");
  const CVarHandle hi = cvar_find("h.i");
  const CVarHandle hf = cvar_find("h.f");
  const CVarHandle hs = cvar_find("h.s");
  bool ok = hb.resolved() && hi.resolved() && hf.resolved() && hs.resolved();
  ok = ok && cvar_handle_live(hb) && cvar_handle_live(hs);
  ok = ok && cvar_get_bool(hb, false) && (cvar_get_int(hi, 0) == 7) &&
       (cvar_get_float(hf, 0.0F) == 2.5F) &&
       (std::strcmp(cvar_get_string(hs, ""), "abc") == 0);

  // Registration stamps at 1; every set advances by one.
  ok = ok && ((cvar_change_stamp(hi) & 0xFFFFFFFFULL) == 1U);
  ok = ok && cvar_set_int("h.i", -3) && (cvar_get_int(hi, 0) == -3);
  ok = ok && ((cvar_change_stamp(hi) & 0xFFFFFFFFULL) == 2U);
  ok = ok && cvar_set_from_string("h.i", "11") && (cvar_get_int(hi, 0) == 11);
  ok = ok && ((cvar_change_stamp(hi) & 0xFFFFFFFFULL) == 3U);
  ok = ok && cvar_set_bool("h.b", false) && !cvar_get_bool(hb, true);
  ok = ok && cvar_set_float("h.f", -0.125F) &&
       (cvar_get_float(hf, 0.0F) == -0.125F);
  const std::uint64_t stringStampBefore = cvar_change_stamp(hs);
  ok = ok && cvar_set_string("h.s", "xyz") &&
       (std::strcmp(cvar_get_string(hs, ""), "xyz") == 0) &&
       (cvar_change_stamp(hs) == stringStampBefore + 1U);
  // A rejected set moves nothing.
  ok = ok && !cvar_set_from_string("h.i", "nope") &&
       ((cvar_change_stamp(hi) & 0xFFFFFFFFULL) == 3U);

  // Type mismatch and unknown names read as the fallback.
  ok = ok && (cvar_get_int(hb, 42) == 42) && !cvar_get_bool(hi, false) &&
       (std::strcmp(cvar_get_string(hi, "fb"), "fb") == 0);
  const CVarHandle none = cvar_find("h.missing");
  ok = ok && !none.resolved() && !cvar_handle_live(none) &&
       (cvar_get_int(none, 9) == 9) && (cvar_change_stamp(none) == 0U);

  shutdown_cvars();
  return ok;
}

/// A handle from before a registry reset reads as its fallback afterwards,
/// even when a different cvar now occupies the same slot; stamps never
/// repeat across the reset.
static bool test_cvar_handle_stale_after_reset() noexcept {
  initialize_cvars();
  cvar_register_int("old.first", 5, "d");
  const CVarHandle stale = cvar_find("old.first");
  const std::uint64_t oldStamp = cvar_change_stamp(stale);
  bool ok = stale.resolved() && (cvar_get_int(stale, -1) == 5);

  shutdown_cvars();
  ok = ok && !cvar_handle_live(stale) && (cvar_get_int(stale, -1) == -1) &&
       (cvar_change_stamp(stale) == 0U);

  initialize_cvars();
  // Slot 0 again, same type, different cvar: the stale handle must not
  // alias it.
  cvar_register_int("new.first", 99, "d");
  const CVarHandle fresh = cvar_find("new.first");
  ok = ok && (fresh.index == stale.index) &&
       (fresh.generation != stale.generation);
  ok = ok && !cvar_handle_live(stale) && (cvar_get_int(stale, -1) == -1) &&
       (cvar_get_int(fresh, -1) == 99);
  ok = ok && (cvar_change_stamp(fresh) != oldStamp) &&
       (cvar_change_stamp(fresh) > oldStamp);

  shutdown_cvars();
  return ok;
}

/// A CVarRef resolves once and then reads without a name scan; it
/// re-resolves after a reset, and an unknown name resolves once the cvar
/// appears.
static bool test_cvar_ref_steady_state_scans_nothing() noexcept {
  initialize_cvars();
  cvar_register_float("ref.f", 1.5F, "d");
  cvar_register_string("ref.s", "one", "d");

  CVarRef ref{"ref.f"};
  bool ok = (ref.get_float(0.0F) == 1.5F);
  const std::size_t afterResolve = cvar_name_lookup_count();
  for (int i = 0; i < 1000; ++i) {
    ok = ok && (ref.get_float(0.0F) == 1.5F);
  }
  ok = ok && (cvar_name_lookup_count() == afterResolve);
  // By-name access is what the counter measures.
  ok = ok && (cvar_get_float("ref.f", 0.0F) == 1.5F) &&
       (cvar_name_lookup_count() == afterResolve + 1U);
  // A set by name is visible on the next handle read.
  ok = ok && cvar_set_float("ref.f", 3.0F) && (ref.get_float(0.0F) == 3.0F);

  // String reads through the reference skip the scan too.
  CVarRef sref{"ref.s"};
  ok = ok && (std::strcmp(sref.get_string(""), "one") == 0);
  const std::size_t afterStringResolve = cvar_name_lookup_count();
  ok = ok && (std::strcmp(sref.get_string(""), "one") == 0) &&
       (sref.change_stamp() != 0U) &&
       (cvar_name_lookup_count() == afterStringResolve);

  // Unknown name: fallback now, resolves once registered.
  CVarRef late{"ref.late"};
  ok = ok && (late.get_int(4) == 4) && (late.change_stamp() == 0U);
  cvar_register_int("ref.late", 8, "d");
  ok = ok && (late.get_int(4) == 8) && (late.change_stamp() != 0U);

  // A null name never resolves and never scans.
  CVarRef none{nullptr};
  const std::size_t beforeNull = cvar_name_lookup_count();
  ok = ok && (none.get_int(6) == 6) && (none.name() == nullptr) &&
       (cvar_name_lookup_count() == beforeNull);

  // Reset: the reference re-resolves against the new registration.
  shutdown_cvars();
  ok = ok && (ref.get_float(-1.0F) == -1.0F);
  initialize_cvars();
  cvar_register_float("ref.f", 7.0F, "d");
  ok = ok && (ref.get_float(-1.0F) == 7.0F);

  shutdown_cvars();
  return ok;
}

/// Concurrent lock-free handle reads against by-name writers: every value
/// a reader observes is one some writer stored (no torn scalars), and the
/// stamp never runs backwards.
static bool test_cvar_handle_reads_under_concurrent_writes() noexcept {
  initialize_cvars();
  cvar_register_float("race.f", 0.0F, "d");
  cvar_register_int("race.i", 0, "d");
  const CVarHandle hf = cvar_find("race.f");
  const CVarHandle hi = cvar_find("race.i");

  std::atomic<bool> ok{true};
  std::atomic<bool> stop{false};
  constexpr int kWrites = 2000;

  std::thread writer([&ok, &stop]() noexcept {
    for (int i = 0; i < kWrites; ++i) {
      // Values whose float bit patterns are all distinct and easy to
      // validate: whole numbers 0..kWrites.
      if (!cvar_set_float("race.f", static_cast<float>(i)) ||
          !cvar_set_int("race.i", i)) {
        ok.store(false, std::memory_order_relaxed);
      }
    }
    stop.store(true, std::memory_order_release);
  });

  constexpr int kReaders = 3;
  std::thread readers[kReaders];
  for (auto &reader : readers) {
    reader = std::thread([&ok, &stop, hf, hi]() noexcept {
      std::uint64_t lastStamp = 0U;
      while (!stop.load(std::memory_order_acquire)) {
        const float f = cvar_get_float(hf, -1.0F);
        const int i = cvar_get_int(hi, -1);
        const std::uint64_t stamp = cvar_change_stamp(hi);
        if ((f < 0.0F) || (f > static_cast<float>(kWrites)) ||
            (f != static_cast<float>(static_cast<int>(f))) || (i < 0) ||
            (i >= kWrites) || (stamp < lastStamp)) {
          ok.store(false, std::memory_order_relaxed);
        }
        lastStamp = stamp;
      }
    });
  }

  writer.join();
  for (auto &reader : readers) {
    reader.join();
  }

  const bool finalOk = (cvar_get_int(hi, -1) == kWrites - 1) &&
                       (cvar_get_float(hf, -1.0F) ==
                        static_cast<float>(kWrites - 1));
  shutdown_cvars();
  return ok.load(std::memory_order_relaxed) && finalOk;
}

static bool test_cvar_console_threaded_access() noexcept {
  initialize_cvars();
  cvar_register_int("thread.i", 0, "threaded int");
  cvar_register_string("thread.s", "start", "threaded string");
  initialize_console();

  std::atomic<int> commandCalls{0};
  std::atomic<bool> ok{true};

  auto command = [](const char *const * /*args*/, int /*argCount*/,
                    void *userData) noexcept {
    auto *counter = static_cast<std::atomic<int> *>(userData);
    counter->fetch_add(1, std::memory_order_relaxed);
    console_print("thread command");
  };

  if (!console_register_command("thread.ping", command, &commandCalls,
                                "thread ping")) {
    shutdown_console();
    shutdown_cvars();
    return false;
  }

  constexpr int kThreadCount = 4;
  constexpr int kIterations = 64;
  std::thread workers[kThreadCount];

  for (int t = 0; t < kThreadCount; ++t) {
    workers[t] = std::thread([t, &ok]() noexcept {
      for (int i = 0; i < kIterations; ++i) {
        char value[32] = {};
        std::snprintf(value, sizeof(value), "t%d-%d", t, i);

        if (!cvar_set_int("thread.i", (t * kIterations) + i)) {
          ok.store(false, std::memory_order_relaxed);
        }
        if (cvar_get_int("thread.i", -1) < 0) {
          ok.store(false, std::memory_order_relaxed);
        }
        if (!cvar_set_string("thread.s", value)) {
          ok.store(false, std::memory_order_relaxed);
        }
        if (cvar_get_string("thread.s", nullptr) == nullptr) {
          ok.store(false, std::memory_order_relaxed);
        }

        CVarInfo cvars[8] = {};
        if (cvar_get_all(cvars, 8U) == 0U) {
          ok.store(false, std::memory_order_relaxed);
        }

        ConsoleCommandInfo commands[8] = {};
        if (console_get_commands(commands, 8U) == 0U) {
          ok.store(false, std::memory_order_relaxed);
        }

        console_print("thread line");
        if (!console_execute("thread.ping")) {
          ok.store(false, std::memory_order_relaxed);
        }
      }
    });
  }

  for (std::thread &worker : workers) {
    worker.join();
  }

  const bool passed =
      ok.load(std::memory_order_relaxed) &&
      (commandCalls.load(std::memory_order_relaxed) ==
       (kThreadCount * kIterations)) &&
      (console_output_line_count() > 0U);

  shutdown_console();
  shutdown_cvars();
  return passed;
}

// ---- entry point ----

int main() {
  struct {
    const char *name;
    bool (*fn)() noexcept;
  } tests[] = {
      {"cvar_register_and_get", test_cvar_register_and_get},
      {"cvar_set", test_cvar_set},
      {"cvar_set_from_string", test_cvar_set_from_string},
      {"cvar_set_from_string_rejects_malformed",
       test_cvar_set_from_string_rejects_malformed},
      {"cvar_duplicate_rejected", test_cvar_duplicate_rejected},
      {"cvar_enumerate", test_cvar_enumerate},
      {"cvar_null_names_after_registration",
       test_cvar_null_names_after_registration},
      {"cvar_handle_reads_match_name", test_cvar_handle_reads_match_name},
      {"cvar_handle_stale_after_reset", test_cvar_handle_stale_after_reset},
      {"cvar_ref_steady_state_scans_nothing",
       test_cvar_ref_steady_state_scans_nothing},
      {"cvar_handle_reads_under_concurrent_writes",
       test_cvar_handle_reads_under_concurrent_writes},
      {"console_basic_execute", test_console_basic_execute},
      {"console_unknown_command", test_console_unknown_command},
      {"console_set_get_cvar", test_console_set_get_cvar},
      {"console_custom_command", test_console_custom_command},
      {"console_output_ring_buffer", test_console_output_ring_buffer},
      {"cvar_console_threaded_access", test_cvar_console_threaded_access},
  };

  int failures = 0;
  for (auto &t : tests) {
    if (!t.fn()) {
      std::printf("FAIL: %s\n", t.name);
      ++failures;
    } else {
      std::printf("PASS: %s\n", t.name);
    }
  }

  return (failures == 0) ? 0 : 1;
}
