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
      {"cvar_duplicate_rejected", test_cvar_duplicate_rejected},
      {"cvar_enumerate", test_cvar_enumerate},
      {"cvar_null_names_after_registration",
       test_cvar_null_names_after_registration},
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
