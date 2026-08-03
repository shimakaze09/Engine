// Verifies input map test behavior for the Engine test suite.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "engine/core/input.h"
#include "engine/core/input_map.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include <SDL3/SDL.h>

using namespace engine::core;

namespace {

// Callback test state.
struct CallbackState {
  int pressedCount = 0;
  int releasedCount = 0;
  char lastName[64] = {};
};

void action_cb(const char *name, bool pressed, void *userData) noexcept {
  auto *state = static_cast<CallbackState *>(userData);
  if (pressed) {
    ++state->pressedCount;
  } else {
    ++state->releasedCount;
  }
  if (name != nullptr) {
    const std::size_t len = std::strlen(name);
    const std::size_t copyLen = (len < sizeof(state->lastName) - 1U)
                                    ? len
                                    : sizeof(state->lastName) - 1U;
    std::memcpy(state->lastName, name, copyLen);
    state->lastName[copyLen] = '\0';
  }
}

struct AxisState {
  float lastValue = 0.0F;
  int callCount = 0;
};

void axis_cb(const char * /*name*/, float value, void *userData) noexcept {
  auto *state = static_cast<AxisState *>(userData);
  state->lastValue = value;
  ++state->callCount;
}

// Helper to init both systems.
bool init_all() noexcept {
  if (!initialize_input()) {
    return false;
  }
  if (!initialize_input_mapper()) {
    shutdown_input();
    return false;
  }
  return true;
}

/// Shuts down the owning system for all.
void shutdown_all() noexcept {
  shutdown_input_mapper();
  shutdown_input();
}

// Helper to simulate a key press event through the full pipeline.
void sim_key_down(KeyScancode key) noexcept {
  SDL_Event ev{};
  ev.type = SDL_EVENT_KEY_DOWN;
  ev.key.scancode = static_cast<SDL_Scancode>(key);
  input_process_event(&ev);
}

void sim_key_up(KeyScancode key) noexcept {
  SDL_Event ev{};
  ev.type = SDL_EVENT_KEY_UP;
  ev.key.scancode = static_cast<SDL_Scancode>(key);
  input_process_event(&ev);
}

/// Writes raw bytes to a file for save fault-injection fixtures.
bool write_raw_file(const char *path, const char *content) noexcept {
  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t len = std::strlen(content);
  const bool ok = (std::fwrite(content, 1U, len, file) == len);
  return (std::fclose(file) == 0) && ok;
}

/// Reads the whole file; empty string when missing.
std::string read_raw_file(const char *path) {
  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "rb");
#endif
  if (file == nullptr) {
    return {};
  }
  char buffer[512] = {};
  const std::size_t read = std::fread(buffer, 1U, sizeof(buffer) - 1U, file);
  std::fclose(file);
  return std::string(buffer, read);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

bool test_add_action_and_poll() noexcept {
  if (!init_all()) {
    return false;
  }

  InputBinding binding{};
  binding.type = InputBindingType::Key;
  binding.code = kKey_Space;

  if (!add_input_action("jump", &binding, 1U)) {
    shutdown_all();
    return false;
  }

  // Not down initially.
  if (is_mapped_action_down("jump")) {
    shutdown_all();
    return false;
  }

  // Press space.
  begin_input_frame();
  sim_key_down(kKey_Space);
  end_input_frame();

  if (!is_mapped_action_down("jump")) {
    shutdown_all();
    return false;
  }

  begin_input_frame();
  sim_key_up(kKey_Space);
  end_input_frame();

  if (is_mapped_action_down("jump")) {
    shutdown_all();
    return false;
  }

  shutdown_all();
  return true;
}

bool test_action_pressed_detection() noexcept {
  if (!init_all()) {
    return false;
  }

  InputBinding binding{};
  binding.type = InputBindingType::Key;
  binding.code = kKey_Space;
  add_input_action("jump", &binding, 1U);

  // Frame 1: press key.
  begin_input_frame();
  sim_key_down(kKey_Space);
  end_input_frame();

  if (!is_mapped_action_pressed("jump")) {
    shutdown_all();
    return false;
  }

  // Frame 2: key still held — should NOT be "pressed" (only on transition).
  begin_input_frame();
  end_input_frame();

  if (is_mapped_action_pressed("jump")) {
    shutdown_all();
    return false;
  }

  shutdown_all();
  return true;
}

bool test_action_callback() noexcept {
  if (!init_all()) {
    return false;
  }

  InputBinding binding{};
  binding.type = InputBindingType::Key;
  binding.code = kKey_A;
  add_input_action("fire", &binding, 1U);

  CallbackState cbState{};
  set_action_callback("fire", &action_cb, &cbState);

  // Frame 1: press A → callback fires "pressed".
  begin_input_frame();
  sim_key_down(kKey_A);
  end_input_frame();

  if (cbState.pressedCount != 1) {
    shutdown_all();
    return false;
  }
  if (std::strcmp(cbState.lastName, "fire") != 0) {
    shutdown_all();
    return false;
  }

  // Frame 2: release A → callback fires "released".
  begin_input_frame();
  sim_key_up(kKey_A);
  end_input_frame();

  if (cbState.releasedCount != 1) {
    shutdown_all();
    return false;
  }

  shutdown_all();
  return true;
}

bool test_multi_binding_action() noexcept {
  if (!init_all()) {
    return false;
  }

  InputBinding bindings[2]{};
  bindings[0].type = InputBindingType::Key;
  bindings[0].code = kKey_Space;
  bindings[1].type = InputBindingType::Key;
  bindings[1].code = kKey_W;

  add_input_action("jump", bindings, 2U);

  // Either key should trigger the action.
  begin_input_frame();
  sim_key_down(kKey_W);
  end_input_frame();

  if (!is_mapped_action_down("jump")) {
    shutdown_all();
    return false;
  }

  begin_input_frame();
  sim_key_up(kKey_W);
  sim_key_down(kKey_Space);
  end_input_frame();

  if (!is_mapped_action_down("jump")) {
    shutdown_all();
    return false;
  }

  shutdown_all();
  return true;
}

bool test_axis_key_pair() noexcept {
  if (!init_all()) {
    return false;
  }

  InputAxisSource src{};
  src.type = AxisSourceType::KeyPair;
  src.negativeKey = kKey_A;
  src.positiveKey = kKey_D;
  src.scale = 1.0F;

  add_input_axis("move_x", &src, 1U);

  // Press D → positive.
  begin_input_frame();
  sim_key_down(kKey_D);
  end_input_frame();

  if (mapped_axis_value("move_x") != 1.0F) {
    shutdown_all();
    return false;
  }

  // Press A (while D still down) → cancel out.
  begin_input_frame();
  sim_key_down(kKey_A);
  end_input_frame();

  if (mapped_axis_value("move_x") != 0.0F) {
    shutdown_all();
    return false;
  }

  begin_input_frame();
  sim_key_up(kKey_D);
  end_input_frame();

  if (mapped_axis_value("move_x") != -1.0F) {
    shutdown_all();
    return false;
  }

  shutdown_all();
  return true;
}

bool test_axis_callback() noexcept {
  if (!init_all()) {
    return false;
  }

  InputAxisSource src{};
  src.type = AxisSourceType::KeyPair;
  src.negativeKey = kKey_A;
  src.positiveKey = kKey_D;
  src.scale = 1.0F;

  add_input_axis("move_x", &src, 1U);

  AxisState axState{};
  set_axis_callback("move_x", &axis_cb, &axState);

  begin_input_frame();
  sim_key_down(kKey_D);
  end_input_frame();

  if (axState.callCount < 1) {
    shutdown_all();
    return false;
  }
  if (axState.lastValue != 1.0F) {
    shutdown_all();
    return false;
  }

  shutdown_all();
  return true;
}

bool test_remove_action() noexcept {
  if (!init_all()) {
    return false;
  }

  InputBinding binding{};
  binding.type = InputBindingType::Key;
  binding.code = kKey_Space;
  add_input_action("jump", &binding, 1U);

  if (!remove_input_action("jump")) {
    shutdown_all();
    return false;
  }

  // Should no longer be found.
  if (is_mapped_action_down("jump")) {
    shutdown_all();
    return false;
  }

  // Removing again should fail.
  if (remove_input_action("jump")) {
    shutdown_all();
    return false;
  }

  shutdown_all();
  return true;
}

bool test_rebind_action() noexcept {
  if (!init_all()) {
    return false;
  }

  InputBinding binding{};
  binding.type = InputBindingType::Key;
  binding.code = kKey_Space;
  add_input_action("jump", &binding, 1U);

  // Rebind to kKey_W.
  InputBinding newBinding{};
  newBinding.type = InputBindingType::Key;
  newBinding.code = kKey_W;
  if (!rebind_action("jump", 0U, newBinding)) {
    shutdown_all();
    return false;
  }

  // Old key should not trigger.
  begin_input_frame();
  sim_key_down(kKey_Space);
  end_input_frame();

  if (is_mapped_action_down("jump")) {
    shutdown_all();
    return false;
  }

  // New key should trigger.
  begin_input_frame();
  sim_key_up(kKey_Space);
  sim_key_down(kKey_W);
  end_input_frame();

  if (!is_mapped_action_down("jump")) {
    shutdown_all();
    return false;
  }

  shutdown_all();
  return true;
}

bool test_save_load_roundtrip() noexcept {
  if (!init_all()) {
    return false;
  }

  InputBinding binding{};
  binding.type = InputBindingType::Key;
  binding.code = kKey_Space;
  add_input_action("jump", &binding, 1U);

  InputAxisSource src{};
  src.type = AxisSourceType::KeyPair;
  src.negativeKey = kKey_A;
  src.positiveKey = kKey_D;
  src.scale = 1.0F;
  add_input_axis("move_x", &src, 1U);

  char buffer[4096] = {};
  std::size_t size = 0U;
  if (!save_input_bindings_to_buffer(buffer, sizeof(buffer), &size)) {
    shutdown_all();
    return false;
  }

  if (size == 0U) {
    shutdown_all();
    return false;
  }

  shutdown_input_mapper();
  initialize_input_mapper();

  if (is_mapped_action_down("jump")) {
    shutdown_all();
    return false;
  }

  if (!load_input_bindings_from_buffer(buffer, size)) {
    shutdown_all();
    return false;
  }

  // Action should be restored (key is still down from before).
  begin_input_frame();
  sim_key_down(kKey_Space);
  end_input_frame();

  if (!is_mapped_action_down("jump")) {
    shutdown_all();
    return false;
  }

  // Axis should be restored.
  begin_input_frame();
  sim_key_up(kKey_Space);
  sim_key_down(kKey_D);
  end_input_frame();

  if (mapped_axis_value("move_x") != 1.0F) {
    shutdown_all();
    return false;
  }

  shutdown_all();
  return true;
}

bool test_null_and_edge_cases() noexcept {
  if (!init_all()) {
    return false;
  }

  // Null name returns false.
  if (add_input_action(nullptr, nullptr, 0U)) {
    shutdown_all();
    return false;
  }
  if (add_input_axis(nullptr, nullptr, 0U)) {
    shutdown_all();
    return false;
  }
  if (is_mapped_action_down(nullptr)) {
    shutdown_all();
    return false;
  }
  if (is_mapped_action_pressed(nullptr)) {
    shutdown_all();
    return false;
  }
  InputBinding binding{};
  binding.type = InputBindingType::Key;
  binding.code = kKey_Space;
  if (!add_input_action("occupied", &binding, 1U)) {
    shutdown_all();
    return false;
  }
  if (is_mapped_action_pressed(nullptr)) {
    shutdown_all();
    return false;
  }
  if (mapped_axis_value(nullptr) != 0.0F) {
    shutdown_all();
    return false;
  }
  if (rebind_action(nullptr, 0U, InputBinding{})) {
    shutdown_all();
    return false;
  }

  // Unknown action.
  if (is_mapped_action_down("nonexistent")) {
    shutdown_all();
    return false;
  }

  shutdown_all();
  return true;
}

/// Round-trips bindings through a real file and checks the default-path
/// shape (composition only — never writes the per-user file).
bool test_file_round_trip_and_default_path() noexcept {
  char defaultPath[512] = {};
  if (engine::core::input_bindings_default_path(defaultPath,
                                                sizeof(defaultPath))) {
    const std::size_t len = std::strlen(defaultPath);
    const char *suffix = "input_bindings.json";
    const std::size_t suffixLen = std::strlen(suffix);
    if ((len <= suffixLen) ||
        (std::strcmp(defaultPath + (len - suffixLen), suffix) != 0) ||
        (std::strchr(defaultPath, '\\') != nullptr)) {
      return false;
    }
  }

  if (!init_all()) {
    return false;
  }

  InputBinding binding{};
  binding.type = InputBindingType::Key;
  binding.code = kKey_Space;
  add_input_action("jump", &binding, 1U);

  const char *tempPath = "input_map_test_bindings.json";
  if (!engine::core::save_input_bindings(tempPath)) {
    shutdown_all();
    return false;
  }

  shutdown_input_mapper();
  initialize_input_mapper();

  const bool loaded = engine::core::load_input_bindings(tempPath);
  static_cast<void>(std::remove(tempPath));
  if (!loaded) {
    shutdown_all();
    return false;
  }

  begin_input_frame();
  sim_key_down(kKey_Space);
  end_input_frame();
  const bool restored = is_mapped_action_down("jump");
  shutdown_all();
  return restored;
}

/// Fault injection (audit N-05): a save whose sibling temporary cannot
/// be staged (read-only parent directory) fails and leaves the
/// pre-existing destination bytes untouched. Skips silently when the
/// environment cannot enforce the read-only fault (e.g. running as
/// root or on Windows directory attributes).
bool test_save_failure_preserves_existing_file() noexcept {
  namespace fs = std::filesystem;
  const char *dirPath = "input_map_test_ro_dir";
  const char *filePath = "input_map_test_ro_dir/bindings.json";
  const char *probePath = "input_map_test_ro_dir/probe.tmp";
  const char *previous = "{\"previous\":true}";

  std::error_code ec{};
  fs::remove_all(dirPath, ec);
  ec.clear();
  if (!fs::create_directory(dirPath, ec) || ec) {
    return false;
  }
  if (!write_raw_file(filePath, previous)) {
    fs::remove_all(dirPath, ec);
    return false;
  }
  fs::permissions(dirPath,
                  fs::perms::owner_read | fs::perms::owner_exec,
                  fs::perm_options::replace, ec);
  if (ec) {
    fs::remove_all(dirPath, ec);
    return false;
  }

  const bool faultActive = !write_raw_file(probePath, "probe");
  bool ok = true;
  if (faultActive) {
    if (!init_all()) {
      ok = false;
    } else {
      InputBinding binding{};
      binding.type = InputBindingType::Key;
      binding.code = kKey_Space;
      add_input_action("jump", &binding, 1U);
      ok = !save_input_bindings(filePath);
      shutdown_all();
    }
    ok = ok && (read_raw_file(filePath) == previous);
  }

  fs::permissions(dirPath, fs::perms::owner_all, fs::perm_options::replace,
                  ec);
  ec.clear();
  static_cast<void>(std::remove(probePath));
  fs::remove_all(dirPath, ec);
  return ok;
}

/// Fault injection (audit N-05): a save whose atomic rename cannot
/// replace its destination (a directory) fails, preserves the
/// directory, and leaves no sibling temporary behind.
bool test_save_to_directory_destination_fails() noexcept {
  namespace fs = std::filesystem;
  const char *dirTarget = "input_map_test_dir_target";
  std::error_code ec{};
  fs::remove_all(dirTarget, ec);
  ec.clear();
  if (!fs::create_directory(dirTarget, ec) || ec) {
    return false;
  }

  bool ok = init_all();
  if (ok) {
    InputBinding binding{};
    binding.type = InputBindingType::Key;
    binding.code = kKey_Space;
    add_input_action("jump", &binding, 1U);
    ok = !save_input_bindings(dirTarget);
    shutdown_all();
  }

  ok = ok && fs::is_directory(dirTarget, ec);
  std::size_t leftovers = 0U;
  for (const auto &entry : fs::directory_iterator(".", ec)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("input_map_test_dir_target.new", 0U) == 0U) {
      ++leftovers;
    }
  }
  fs::remove_all(dirTarget, ec);
  return ok && (leftovers == 0U);
}

} // namespace

/// Runs this executable or test program.
int main() {
  int passed = 0;
  int failed = 0;

  auto run = [&](const char *name, bool (*fn)() noexcept) {
    if (fn()) {
      ++passed;
      std::printf("  PASS  %s\n", name);
    } else {
      ++failed;
      std::printf("  FAIL  %s\n", name);
    }
  };

  std::printf("--- input_map tests ---\n");
  run("add_action_and_poll", &test_add_action_and_poll);
  run("action_pressed_detection", &test_action_pressed_detection);
  run("action_callback", &test_action_callback);
  run("multi_binding_action", &test_multi_binding_action);
  run("axis_key_pair", &test_axis_key_pair);
  run("axis_callback", &test_axis_callback);
  run("remove_action", &test_remove_action);
  run("rebind_action", &test_rebind_action);
  run("save_load_roundtrip", &test_save_load_roundtrip);
  run("file_round_trip_and_default_path",
      &test_file_round_trip_and_default_path);
  run("save_failure_preserves_existing_file",
      &test_save_failure_preserves_existing_file);
  run("save_to_directory_destination_fails",
      &test_save_to_directory_destination_fails);
  run("null_and_edge_cases", &test_null_and_edge_cases);

  std::printf("--- %d passed, %d failed ---\n", passed, failed);
  return (failed > 0) ? 1 : 0;
}
