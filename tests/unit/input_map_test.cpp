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

/// Entry-level rejection (audit M-11): entries with missing/empty/overlong
/// names, non-object entries, and out-of-range binding/source type enums
/// refuse the whole load, and the current bindings survive untouched.
bool test_invalid_entry_load_preserves_bindings() noexcept {
  if (!init_all()) {
    return false;
  }

  InputBinding binding{};
  binding.type = InputBindingType::Key;
  binding.code = kKey_Space;
  add_input_action("jump", &binding, 1U);

  char overlongDoc[512] = {};
  {
    char longName[80] = {};
    for (std::size_t i = 0U; i < 64U; ++i) {
      longName[i] = 'n';
    }
    std::snprintf(overlongDoc, sizeof(overlongDoc),
                  "{\"actions\":[{\"name\":\"%s\",\"bindings\":[]}],"
                  "\"axes\":[]}",
                  longName);
  }

  const char *rejected[] = {
      "{\"actions\":[7],\"axes\":[]}",
      "{\"actions\":[{\"bindings\":[]}],\"axes\":[]}",
      "{\"actions\":[{\"name\":\"\",\"bindings\":[]}],\"axes\":[]}",
      "{\"actions\":[{\"name\":\"a\",\"bindings\":[3]}],\"axes\":[]}",
      "{\"actions\":[{\"name\":\"a\",\"bindings\":[{\"type\":4,"
      "\"code\":1}]}],\"axes\":[]}",
      "{\"actions\":[],\"axes\":[{\"sources\":[]}]}",
      "{\"actions\":[],\"axes\":[{\"name\":\"m\",\"sources\":[{\"type\":7}]}"
      "]}",
      overlongDoc,
  };
  for (const char *doc : rejected) {
    if (load_input_bindings_from_buffer(doc, std::strlen(doc))) {
      shutdown_all();
      return false;
    }
  }

  begin_input_frame();
  sim_key_down(kKey_Space);
  end_input_frame();
  const bool actionIntact = is_mapped_action_down("jump");
  begin_input_frame();
  sim_key_up(kKey_Space);
  end_input_frame();

  shutdown_all();
  return actionIntact;
}

/// Wrong-shape rejection (audit P2-9): valid JSON without the expected
/// top-level actions/axes arrays — including "{}" — is refused and the
/// current bindings survive untouched; a well-formed document still
/// loads afterwards.
bool test_wrong_shape_load_preserves_bindings() noexcept {
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

  const char *rejected[] = {
      "{}",
      "{\"actions\":3,\"axes\":[]}",
      "{\"actions\":[],\"axes\":{}}",
      "{\"wrong\":[]}",
      "[1,2,3]",
      "{\"actions\":[",
  };
  for (const char *doc : rejected) {
    if (load_input_bindings_from_buffer(doc, std::strlen(doc))) {
      shutdown_all();
      return false;
    }
  }

  begin_input_frame();
  sim_key_down(kKey_Space);
  sim_key_down(kKey_D);
  end_input_frame();
  const bool actionIntact = is_mapped_action_down("jump");
  const bool axisIntact = mapped_axis_value("move_x") == 1.0F;
  begin_input_frame();
  sim_key_up(kKey_Space);
  sim_key_up(kKey_D);
  end_input_frame();
  if (!actionIntact || !axisIntact) {
    shutdown_all();
    return false;
  }

  const char *valid = "{\"actions\":[],\"axes\":[]}";
  if (!load_input_bindings_from_buffer(valid, std::strlen(valid))) {
    shutdown_all();
    return false;
  }
  const bool cleared = !is_mapped_action_down("jump");
  shutdown_all();
  return cleared;
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

/// Over-capacity authored documents are rejected whole (issue #385): a
/// syntactically valid document with 65 actions, nine bindings on one
/// action, 65 axes, or nine sources on one axis must fail the load and
/// leave the live mappings untouched — silently dropping the overflow
/// entries would let the next save make that loss permanent. Documents at
/// exactly the four capacities still load and round-trip byte-identically.
bool test_over_capacity_load_rejected_whole() noexcept {
  if (!init_all()) {
    return false;
  }

  InputBinding binding{};
  binding.type = InputBindingType::Key;
  binding.code = kKey_Space;
  add_input_action("jump", &binding, 1U);

  // One action carrying 65 sibling entries, nine bindings on one action,
  // 65 axes, and nine sources on one axis, each in an otherwise valid
  // document.
  std::string overActions = "{\"actions\":[";
  for (int i = 0; i < 65; ++i) {
    char entry[64] = {};
    std::snprintf(entry, sizeof(entry), "%s{\"name\":\"a%d\",\"bindings\":[]}",
                  (i > 0) ? "," : "", i);
    overActions += entry;
  }
  overActions += "],\"axes\":[]}";

  std::string overBindings = "{\"actions\":[{\"name\":\"many\",\"bindings\":[";
  for (int i = 0; i < 9; ++i) {
    char entry[64] = {};
    std::snprintf(entry, sizeof(entry), "%s{\"type\":0,\"code\":%d}",
                  (i > 0) ? "," : "", 40 + i);
    overBindings += entry;
  }
  overBindings += "]}],\"axes\":[]}";

  std::string overAxes = "{\"actions\":[],\"axes\":[";
  for (int i = 0; i < 65; ++i) {
    char entry[64] = {};
    std::snprintf(entry, sizeof(entry), "%s{\"name\":\"x%d\",\"sources\":[]}",
                  (i > 0) ? "," : "", i);
    overAxes += entry;
  }
  overAxes += "]}";

  std::string overSources = "{\"actions\":[],\"axes\":[{\"name\":\"m\",\"sources\":[";
  for (int i = 0; i < 9; ++i) {
    char entry[96] = {};
    std::snprintf(entry, sizeof(entry),
                  "%s{\"type\":0,\"negative_key\":4,\"positive_key\":7,"
                  "\"scale\":1.0}",
                  (i > 0) ? "," : "");
    overSources += entry;
  }
  overSources += "]}]}";

  const std::string *rejected[] = {&overActions, &overBindings, &overAxes,
                                   &overSources};
  for (const std::string *doc : rejected) {
    if (load_input_bindings_from_buffer(doc->c_str(), doc->size())) {
      shutdown_all();
      return false;
    }
  }

  // The live mappings survived all four refusals.
  begin_input_frame();
  sim_key_down(kKey_Space);
  end_input_frame();
  const bool actionIntact = is_mapped_action_down("jump");
  begin_input_frame();
  sim_key_up(kKey_Space);
  end_input_frame();
  if (!actionIntact) {
    shutdown_all();
    return false;
  }

  // Exact-capacity boundary: fill all four capacities through the public
  // API, save, reload, and save again — the two documents must be
  // byte-identical, proving capacity-sized data loads whole and round-trips
  // unchanged.
  shutdown_input_mapper();
  initialize_input_mapper();

  InputBinding fullBindings[kMaxBindingsPerAction] = {};
  for (std::size_t b = 0; b < kMaxBindingsPerAction; ++b) {
    fullBindings[b].type = InputBindingType::Key;
    fullBindings[b].code = static_cast<int>(30U + b);
  }
  InputAxisSource fullSources[kMaxSourcesPerAxis] = {};
  for (std::size_t v = 0; v < kMaxSourcesPerAxis; ++v) {
    fullSources[v].type = AxisSourceType::KeyPair;
    fullSources[v].negativeKey = static_cast<int>(4U + v);
    fullSources[v].positiveKey = static_cast<int>(20U + v);
    fullSources[v].scale = 1.0F;
  }
  char name[16] = {};
  for (std::size_t i = 0; i < kMaxInputActions; ++i) {
    std::snprintf(name, sizeof(name), "act%zu", i);
    if (!add_input_action(name, fullBindings,
                          (i == 0U) ? kMaxBindingsPerAction : 1U)) {
      shutdown_all();
      return false;
    }
  }
  for (std::size_t i = 0; i < kMaxInputAxes; ++i) {
    std::snprintf(name, sizeof(name), "axis%zu", i);
    if (!add_input_axis(name, fullSources,
                        (i == 0U) ? kMaxSourcesPerAxis : 1U)) {
      shutdown_all();
      return false;
    }
  }

  static char firstSave[128U * 1024U] = {};
  static char secondSave[128U * 1024U] = {};
  std::size_t firstSize = 0U;
  std::size_t secondSize = 0U;
  if (!save_input_bindings_to_buffer(firstSave, sizeof(firstSave),
                                     &firstSize)) {
    shutdown_all();
    return false;
  }

  shutdown_input_mapper();
  initialize_input_mapper();
  if (!load_input_bindings_from_buffer(firstSave, firstSize)) {
    shutdown_all();
    return false;
  }
  if (!save_input_bindings_to_buffer(secondSave, sizeof(secondSave),
                                     &secondSize)) {
    shutdown_all();
    return false;
  }

  const bool roundTripped = (firstSize == secondSize) &&
                            (std::memcmp(firstSave, secondSave, firstSize) == 0);
  shutdown_all();
  return roundTripped;
}

/// Inert probe callbacks: registration succeeds only for an existing entry,
/// and neither reads the (null) user data if the mapper later fires it.
void probe_action_cb(const char * /*name*/, bool /*pressed*/,
                     void * /*userData*/) noexcept {}
void probe_axis_cb(const char * /*name*/, float /*value*/,
                   void * /*userData*/) noexcept {}

/// Returns whether the mapper holds an action under exactly `name`, using
/// callback registration as the lookup probe.
bool has_action_named(const char *name) noexcept {
  return set_action_callback(name, &probe_action_cb, nullptr);
}

/// Returns whether the mapper holds an axis under exactly `name`.
bool has_axis_named(const char *name) noexcept {
  return set_axis_callback(name, &probe_axis_cb, nullptr);
}

/// Loads `doc` into a fresh mapper (init_all already ran).
bool load_doc(const char *doc) noexcept {
  return load_input_bindings_from_buffer(doc, std::strlen(doc));
}

/// Authored names carrying JSON escapes must load as their decoded text
/// (issue #386): an escaped quote, a backslash, and a BMP \u escape are
/// looked up by the logical name, the saved document escapes them exactly
/// once, and load/save/load is byte-stable.
bool test_escaped_names_decode_and_round_trip() noexcept {
  if (!init_all()) {
    return false;
  }

  // Decoded names: say "hi" | back\slash | jump<e-acute> (U+00E9, UTF-8
  // C3 A9) | move<tab>x.
  const char *doc =
      "{\"actions\":["
      "{\"name\":\"say \\\"hi\\\"\",\"bindings\":[{\"type\":0,\"code\":44}]},"
      "{\"name\":\"back\\\\slash\",\"bindings\":[]},"
      "{\"name\":\"jump\\u00e9\",\"bindings\":[]}"
      "],\"axes\":["
      "{\"name\":\"move\\tx\",\"sources\":[]},"
      "{\"name\":\"axis\\u00e9\",\"sources\":[]}"
      "]}";
  if (!load_doc(doc)) {
    shutdown_all();
    return false;
  }

  const bool decodedLookup =
      has_action_named("say \"hi\"") && has_action_named("back\\slash") &&
      has_action_named("jump\xC3\xA9") && has_axis_named("move\tx") &&
      has_axis_named("axis\xC3\xA9");
  // The encoded token bytes must not be a runtime identity.
  const bool encodedRejected = !has_action_named("say \\\"hi\\\"") &&
                               !has_action_named("back\\\\slash") &&
                               !has_action_named("jump\\u00e9") &&
                               !has_axis_named("move\\tx");
  if (!decodedLookup || !encodedRejected) {
    shutdown_all();
    return false;
  }

  // The decoded name reaches callbacks and polling by its logical text.
  CallbackState cbState{};
  set_action_callback("say \"hi\"", &action_cb, &cbState);
  begin_input_frame();
  sim_key_down(kKey_Space);
  end_input_frame();
  const bool polled = is_mapped_action_down("say \"hi\"");
  begin_input_frame();
  sim_key_up(kKey_Space);
  end_input_frame();
  if (!polled || (cbState.pressedCount != 1) ||
      (std::strcmp(cbState.lastName, "say \"hi\"") != 0)) {
    shutdown_all();
    return false;
  }

  // Save escapes each name exactly once: the quote appears as \" (not
  // \\\"), the backslash as \\ (not \\\\), and the UTF-8 bytes verbatim.
  char firstSave[4096] = {};
  std::size_t firstSize = 0U;
  if (!save_input_bindings_to_buffer(firstSave, sizeof(firstSave),
                                     &firstSize)) {
    shutdown_all();
    return false;
  }
  const bool escapedOnce =
      (std::strstr(firstSave, "\"say \\\"hi\\\"\"") != nullptr) &&
      (std::strstr(firstSave, "\\\\\\\"") == nullptr) &&
      (std::strstr(firstSave, "\"back\\\\slash\"") != nullptr) &&
      (std::strstr(firstSave, "\\\\\\\\") == nullptr) &&
      (std::strstr(firstSave, "jump\xC3\xA9") != nullptr) &&
      (std::strstr(firstSave, "\\u00e9") == nullptr);
  if (!escapedOnce) {
    shutdown_all();
    return false;
  }

  // Load/save/load: byte-identical document and identical runtime names.
  shutdown_input_mapper();
  initialize_input_mapper();
  char secondSave[4096] = {};
  std::size_t secondSize = 0U;
  if (!load_input_bindings_from_buffer(firstSave, firstSize) ||
      !save_input_bindings_to_buffer(secondSave, sizeof(secondSave),
                                     &secondSize)) {
    shutdown_all();
    return false;
  }
  const bool stable = (firstSize == secondSize) &&
                      (std::memcmp(firstSave, secondSave, firstSize) == 0) &&
                      has_action_named("say \"hi\"") &&
                      has_action_named("back\\slash") &&
                      has_action_named("jump\xC3\xA9") &&
                      has_axis_named("move\tx") &&
                      has_axis_named("axis\xC3\xA9");
  shutdown_all();
  return stable;
}

/// Malformed escapes in a name reject the whole load and leave the live
/// mappings untouched (issue #386): an unknown escape, a short \u, a lone
/// high surrogate, an unpaired low surrogate, and \u0000.
bool test_malformed_escape_names_rejected() noexcept {
  if (!init_all()) {
    return false;
  }

  InputBinding binding{};
  binding.type = InputBindingType::Key;
  binding.code = kKey_Space;
  add_input_action("jump", &binding, 1U);

  const char *rejected[] = {
      "{\"actions\":[{\"name\":\"bad\\x\",\"bindings\":[]}],\"axes\":[]}",
      "{\"actions\":[{\"name\":\"bad\\u12\",\"bindings\":[]}],\"axes\":[]}",
      "{\"actions\":[{\"name\":\"bad\\ud800\",\"bindings\":[]}],\"axes\":[]}",
      "{\"actions\":[{\"name\":\"bad\\udc00\",\"bindings\":[]}],\"axes\":[]}",
      "{\"actions\":[{\"name\":\"bad\\u0000\",\"bindings\":[]}],\"axes\":[]}",
      "{\"actions\":[],\"axes\":[{\"name\":\"bad\\q\",\"sources\":[]}]}",
      "{\"actions\":[],\"axes\":[{\"name\":\"\\ud800x\",\"sources\":[]}]}",
  };
  for (const char *doc : rejected) {
    if (load_doc(doc)) {
      shutdown_all();
      return false;
    }
  }

  begin_input_frame();
  sim_key_down(kKey_Space);
  end_input_frame();
  const bool actionIntact = is_mapped_action_down("jump");
  begin_input_frame();
  sim_key_up(kKey_Space);
  end_input_frame();
  shutdown_all();
  return actionIntact;
}

/// The name-length limit applies to the decoded text (issue #386): a name
/// whose escaped form is longer than the slot but decodes to exactly
/// kMaxInputNameLen bytes loads whole, one decoded byte more is refused
/// (whether the overflow comes from an escape or a multi-byte \u), and an
/// escape-only name that decodes to nothing is refused as empty.
bool test_decoded_name_length_boundaries() noexcept {
  if (!init_all()) {
    return false;
  }

  // 61 plain bytes + \" + \" = 63 decoded (65 encoded); 62 + two escapes
  // = 64 decoded.
  std::string atLimit = "{\"actions\":[{\"name\":\"";
  std::string overLimit = atLimit;
  atLimit.append(kMaxInputNameLen - 2U, 'n');
  overLimit.append(kMaxInputNameLen - 1U, 'n');
  atLimit += "\\\"\\\"\",\"bindings\":[]}],\"axes\":[]}";
  overLimit += "\\\"\\\"\",\"bindings\":[]}],\"axes\":[]}";

  // 31 x U+00E9 (two UTF-8 bytes each) + one plain byte = 63 decoded; 32 x
  // U+00E9 = 64 decoded, both far shorter than the slot when encoded.
  std::string utf8AtLimit = "{\"actions\":[],\"axes\":[{\"name\":\"";
  std::string utf8OverLimit = utf8AtLimit;
  for (std::size_t i = 0U; i < 31U; ++i) {
    utf8AtLimit += "\\u00e9";
    utf8OverLimit += "\\u00e9";
  }
  utf8AtLimit += "n";
  utf8OverLimit += "\\u00e9";
  utf8AtLimit += "\",\"sources\":[]}]}";
  utf8OverLimit += "\",\"sources\":[]}]}";

  if (!load_doc(atLimit.c_str())) {
    shutdown_all();
    return false;
  }
  std::string expected(kMaxInputNameLen - 2U, 'n');
  expected += "\"\"";
  if (!has_action_named(expected.c_str())) {
    shutdown_all();
    return false;
  }
  if (!load_doc(utf8AtLimit.c_str())) {
    shutdown_all();
    return false;
  }
  std::string expectedAxis;
  for (std::size_t i = 0U; i < 31U; ++i) {
    expectedAxis += "\xC3\xA9";
  }
  expectedAxis += "n";
  if (!has_axis_named(expectedAxis.c_str())) {
    shutdown_all();
    return false;
  }

  const std::string *rejected[] = {&overLimit, &utf8OverLimit};
  for (const std::string *doc : rejected) {
    if (load_doc(doc->c_str())) {
      shutdown_all();
      return false;
    }
  }
  // The over-limit refusals left the last accepted document in place.
  const bool intact = has_axis_named(expectedAxis.c_str());
  shutdown_all();
  return intact;
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
  run("wrong_shape_load_preserves_bindings",
      &test_wrong_shape_load_preserves_bindings);
  run("invalid_entry_load_preserves_bindings",
      &test_invalid_entry_load_preserves_bindings);
  run("save_failure_preserves_existing_file",
      &test_save_failure_preserves_existing_file);
  run("save_to_directory_destination_fails",
      &test_save_to_directory_destination_fails);
  run("over_capacity_load_rejected_whole",
      &test_over_capacity_load_rejected_whole);
  run("escaped_names_decode_and_round_trip",
      &test_escaped_names_decode_and_round_trip);
  run("malformed_escape_names_rejected",
      &test_malformed_escape_names_rejected);
  run("decoded_name_length_boundaries",
      &test_decoded_name_length_boundaries);
  run("null_and_edge_cases", &test_null_and_edge_cases);

  std::printf("--- %d passed, %d failed ---\n", passed, failed);
  return (failed > 0) ? 1 : 0;
}
