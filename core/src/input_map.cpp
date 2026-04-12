#include "engine/core/input_map.h"
#include "engine/core/input.h"
#include "engine/core/json.h"
#include "engine/core/logging.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace engine::core {

namespace {

constexpr const char *kLogChannel = "InputMapper";

bool g_mapperInitialized = false;

std::array<InputAction, kMaxInputActions> g_mappedActions{};
std::array<InputAxisMapping, kMaxInputAxes> g_mappedAxes{};

// Previous-frame action state for pressed/released detection.
std::array<bool, kMaxInputActions> g_actionDown{};
std::array<bool, kMaxInputActions> g_prevActionDown{};

// Current-frame mouse delta (accumulated).
float g_mouseDeltaX = 0.0F;
float g_mouseDeltaY = 0.0F;

InputAction *find_mapped_action(const char *name) noexcept {
  if (name == nullptr) {
    return nullptr;
  }
  for (auto &a : g_mappedActions) {
    if (a.occupied && (std::strcmp(a.name, name) == 0)) {
      return &a;
    }
  }
  return nullptr;
}

InputAxisMapping *find_mapped_axis(const char *name) noexcept {
  if (name == nullptr) {
    return nullptr;
  }
  for (auto &a : g_mappedAxes) {
    if (a.occupied && (std::strcmp(a.name, name) == 0)) {
      return &a;
    }
  }
  return nullptr;
}

bool evaluate_binding(const InputBinding &binding) noexcept {
  switch (binding.type) {
  case InputBindingType::Key:
    return (binding.code >= 0) && is_key_down(binding.code);
  case InputBindingType::MouseButton:
    return (binding.code >= 0) && is_mouse_button_down(binding.code);
  case InputBindingType::GamepadButton:
    return is_gamepad_connected() && is_gamepad_button_down(binding.code);
  case InputBindingType::GamepadAxis: {
    if (!is_gamepad_connected()) {
      return false;
    }
    const float raw = gamepad_axis_value(binding.code);
    const float scaled = raw * binding.axisScale;
    return (scaled >= binding.axisThreshold) ||
           (scaled <= -binding.axisThreshold);
  }
  }
  return false;
}

float evaluate_axis_source(const InputAxisSource &src) noexcept {
  switch (src.type) {
  case AxisSourceType::KeyPair: {
    const bool neg = (src.negativeKey >= 0) && is_key_down(src.negativeKey);
    const bool pos = (src.positiveKey >= 0) && is_key_down(src.positiveKey);
    if (neg == pos) {
      return 0.0F;
    }
    return (pos ? 1.0F : -1.0F) * src.scale;
  }
  case AxisSourceType::GamepadAxis: {
    if (!is_gamepad_connected() || (src.axisIndex < 0)) {
      return 0.0F;
    }
    const int rawDeadZone = static_cast<int>(src.deadZone * 32767.0F);
    const float raw = gamepad_axis_value(src.axisIndex, rawDeadZone);
    return raw * src.scale;
  }
  case AxisSourceType::MouseDeltaX:
    return g_mouseDeltaX * src.scale;
  case AxisSourceType::MouseDeltaY:
    return g_mouseDeltaY * src.scale;
  }
  return 0.0F;
}

// File I/O helpers (platform-agnostic, used for JSON persistence).
bool open_file_for_read(const char *path, FILE **outFile) noexcept {
  if ((path == nullptr) || (outFile == nullptr)) {
    return false;
  }
#if defined(_MSC_VER) || defined(_WIN32)
  *outFile = nullptr;
  return fopen_s(outFile, path, "rb") == 0;
#else
  *outFile = std::fopen(path, "rb");
  return *outFile != nullptr;
#endif
}

bool open_file_for_write(const char *path, FILE **outFile) noexcept {
  if ((path == nullptr) || (outFile == nullptr)) {
    return false;
  }
#if defined(_MSC_VER) || defined(_WIN32)
  *outFile = nullptr;
  return fopen_s(outFile, path, "wb") == 0;
#else
  *outFile = std::fopen(path, "wb");
  return *outFile != nullptr;
#endif
}

bool read_text_file(const char *path, std::unique_ptr<char[]> *outBuffer,
                    std::size_t *outSize) noexcept {
  FILE *file = nullptr;
  if (!open_file_for_read(path, &file) || (file == nullptr)) {
    return false;
  }
  std::fseek(file, 0, SEEK_END);
  const long fileSize = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  if (fileSize <= 0) {
    std::fclose(file);
    return false;
  }
  const auto size = static_cast<std::size_t>(fileSize);
  std::unique_ptr<char[]> buffer(new (std::nothrow) char[size + 1U]);
  if (buffer == nullptr) {
    std::fclose(file);
    return false;
  }
  const std::size_t readCount = std::fread(buffer.get(), 1U, size, file);
  const bool hitError = std::ferror(file) != 0;
  std::fclose(file);
  if (hitError || (readCount != size)) {
    return false;
  }
  buffer[size] = '\0';
  *outSize = size;
  outBuffer->swap(buffer);
  return true;
}

bool write_text_file(const char *path, const char *text,
                     std::size_t size) noexcept {
  if ((path == nullptr) || (text == nullptr) || (size == 0U)) {
    return false;
  }
  FILE *file = nullptr;
  if (!open_file_for_write(path, &file) || (file == nullptr)) {
    return false;
  }
  const std::size_t written = std::fwrite(text, 1U, size, file);
  std::fclose(file);
  return written == size;
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool initialize_input_mapper() noexcept {
  if (g_mapperInitialized) {
    return true;
  }
  g_mappedActions = {};
  g_mappedAxes = {};
  g_actionDown = {};
  g_prevActionDown = {};
  g_mouseDeltaX = 0.0F;
  g_mouseDeltaY = 0.0F;
  g_mapperInitialized = true;
  return true;
}

void shutdown_input_mapper() noexcept {
  g_mapperInitialized = false;
  g_mappedActions = {};
  g_mappedAxes = {};
  g_actionDown = {};
  g_prevActionDown = {};
  g_mouseDeltaX = 0.0F;
  g_mouseDeltaY = 0.0F;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

bool add_input_action(const char *name, const InputBinding *bindings,
                      std::uint32_t count) noexcept {
  if (name == nullptr) {
    return false;
  }
  const std::size_t nameLen = std::strlen(name);
  if ((nameLen == 0U) || (nameLen > kMaxInputNameLen)) {
    return false;
  }
  if ((count > 0U) && (bindings == nullptr)) {
    return false;
  }
  if (count > kMaxBindingsPerAction) {
    count = static_cast<std::uint32_t>(kMaxBindingsPerAction);
  }

  // Overwrite existing?
  InputAction *existing = find_mapped_action(name);
  if (existing != nullptr) {
    existing->bindingCount = count;
    for (std::uint32_t i = 0; i < count; ++i) {
      existing->bindings[i] = bindings[i];
    }
    return true;
  }

  // Find empty slot.
  for (auto &a : g_mappedActions) {
    if (!a.occupied) {
      std::memcpy(a.name, name, nameLen + 1U);
      a.bindingCount = count;
      for (std::uint32_t i = 0; i < count; ++i) {
        a.bindings[i] = bindings[i];
      }
      a.callback = nullptr;
      a.userData = nullptr;
      a.occupied = true;
      return true;
    }
  }

  log_message(LogLevel::Warning, kLogChannel,
              "add_input_action: no free action slots");
  return false;
}

bool add_input_axis(const char *name, const InputAxisSource *sources,
                    std::uint32_t count) noexcept {
  if (name == nullptr) {
    return false;
  }
  const std::size_t nameLen = std::strlen(name);
  if ((nameLen == 0U) || (nameLen > kMaxInputNameLen)) {
    return false;
  }
  if ((count > 0U) && (sources == nullptr)) {
    return false;
  }
  if (count > kMaxSourcesPerAxis) {
    count = static_cast<std::uint32_t>(kMaxSourcesPerAxis);
  }

  // Overwrite existing?
  InputAxisMapping *existing = find_mapped_axis(name);
  if (existing != nullptr) {
    existing->sourceCount = count;
    for (std::uint32_t i = 0; i < count; ++i) {
      existing->sources[i] = sources[i];
    }
    return true;
  }

  // Find empty slot.
  for (auto &a : g_mappedAxes) {
    if (!a.occupied) {
      std::memcpy(a.name, name, nameLen + 1U);
      a.sourceCount = count;
      for (std::uint32_t i = 0; i < count; ++i) {
        a.sources[i] = sources[i];
      }
      a.callback = nullptr;
      a.userData = nullptr;
      a.occupied = true;
      return true;
    }
  }

  log_message(LogLevel::Warning, kLogChannel,
              "add_input_axis: no free axis slots");
  return false;
}

bool remove_input_action(const char *name) noexcept {
  InputAction *a = find_mapped_action(name);
  if (a == nullptr) {
    return false;
  }
  *a = InputAction{};
  return true;
}

bool remove_input_axis(const char *name) noexcept {
  InputAxisMapping *a = find_mapped_axis(name);
  if (a == nullptr) {
    return false;
  }
  *a = InputAxisMapping{};
  return true;
}

// ---------------------------------------------------------------------------
// Callback registration
// ---------------------------------------------------------------------------

bool set_action_callback(const char *name, ActionCallback cb,
                         void *userData) noexcept {
  InputAction *a = find_mapped_action(name);
  if (a == nullptr) {
    return false;
  }
  a->callback = cb;
  a->userData = userData;
  return true;
}

bool set_axis_callback(const char *name, AxisCallback cb,
                       void *userData) noexcept {
  InputAxisMapping *a = find_mapped_axis(name);
  if (a == nullptr) {
    return false;
  }
  a->callback = cb;
  a->userData = userData;
  return true;
}

// ---------------------------------------------------------------------------
// Polling
// ---------------------------------------------------------------------------

bool is_mapped_action_down(const char *name) noexcept {
  const InputAction *a = find_mapped_action(name);
  if (a == nullptr) {
    return false;
  }
  for (std::uint32_t i = 0; i < a->bindingCount; ++i) {
    if (evaluate_binding(a->bindings[i])) {
      return true;
    }
  }
  return false;
}

bool is_mapped_action_pressed(const char *name) noexcept {
  // Find the action index to compare current vs previous frame.
  for (std::size_t i = 0; i < kMaxInputActions; ++i) {
    if (g_mappedActions[i].occupied &&
        (std::strcmp(g_mappedActions[i].name, name) == 0)) {
      return g_actionDown[i] && !g_prevActionDown[i];
    }
  }
  return false;
}

float mapped_axis_value(const char *name) noexcept {
  const InputAxisMapping *a = find_mapped_axis(name);
  if (a == nullptr) {
    return 0.0F;
  }
  // Combine all sources — take the one with the largest absolute value.
  float result = 0.0F;
  for (std::uint32_t i = 0; i < a->sourceCount; ++i) {
    const float val = evaluate_axis_source(a->sources[i]);
    if ((val > 0.0F ? val : -val) > (result > 0.0F ? result : -result)) {
      result = val;
    }
  }
  return result;
}

// ---------------------------------------------------------------------------
// Runtime rebinding
// ---------------------------------------------------------------------------

bool rebind_action(const char *actionName, std::uint32_t bindingIndex,
                   const InputBinding &newBinding) noexcept {
  InputAction *a = find_mapped_action(actionName);
  if (a == nullptr) {
    return false;
  }
  if (bindingIndex >= a->bindingCount) {
    // Allow extending by one if there's room.
    if ((bindingIndex == a->bindingCount) &&
        (a->bindingCount < kMaxBindingsPerAction)) {
      a->bindings[a->bindingCount] = newBinding;
      ++a->bindingCount;
      return true;
    }
    return false;
  }
  a->bindings[bindingIndex] = newBinding;
  return true;
}

// ---------------------------------------------------------------------------
// Per-frame processing
// ---------------------------------------------------------------------------

void input_mapper_begin_frame() noexcept {
  g_prevActionDown = g_actionDown;
  g_mouseDeltaX = 0.0F;
  g_mouseDeltaY = 0.0F;
}

void input_mapper_process_event(const void *nativeEvent) noexcept {
  if (nativeEvent == nullptr) {
    return;
  }
  // Mouse motion events update the per-frame delta accumulator.
  // All other event types are handled indirectly via the underlying input
  // state (is_key_down, etc.) which is updated by input_process_event.
  const MouseState ms = mouse_state();
  g_mouseDeltaX = static_cast<float>(ms.deltaX);
  g_mouseDeltaY = static_cast<float>(ms.deltaY);
}

void input_mapper_end_frame() noexcept {
  // Evaluate all mapped actions and record their current state.
  for (std::size_t i = 0; i < kMaxInputActions; ++i) {
    if (!g_mappedActions[i].occupied) {
      g_actionDown[i] = false;
      continue;
    }
    bool down = false;
    for (std::uint32_t b = 0; b < g_mappedActions[i].bindingCount; ++b) {
      if (evaluate_binding(g_mappedActions[i].bindings[b])) {
        down = true;
        break;
      }
    }
    g_actionDown[i] = down;

    // Fire callback on press/release transitions.
    if (g_mappedActions[i].callback != nullptr) {
      if (down && !g_prevActionDown[i]) {
        g_mappedActions[i].callback(g_mappedActions[i].name, true,
                                    g_mappedActions[i].userData);
      } else if (!down && g_prevActionDown[i]) {
        g_mappedActions[i].callback(g_mappedActions[i].name, false,
                                    g_mappedActions[i].userData);
      }
    }
  }

  // Fire axis callbacks for all mapped axes.
  for (std::size_t i = 0; i < kMaxInputAxes; ++i) {
    if (!g_mappedAxes[i].occupied || (g_mappedAxes[i].callback == nullptr)) {
      continue;
    }
    float result = 0.0F;
    for (std::uint32_t s = 0; s < g_mappedAxes[i].sourceCount; ++s) {
      const float val = evaluate_axis_source(g_mappedAxes[i].sources[s]);
      if ((val > 0.0F ? val : -val) > (result > 0.0F ? result : -result)) {
        result = val;
      }
    }
    g_mappedAxes[i].callback(g_mappedAxes[i].name, result,
                             g_mappedAxes[i].userData);
  }
}

// ---------------------------------------------------------------------------
// JSON persistence
// ---------------------------------------------------------------------------

bool save_input_bindings(const char *path) noexcept {
  JsonWriter writer{};
  writer.begin_object();

  // Actions array.
  writer.begin_array("actions");
  for (std::size_t i = 0; i < kMaxInputActions; ++i) {
    if (!g_mappedActions[i].occupied) {
      continue;
    }
    writer.begin_object();
    writer.write_string("name", g_mappedActions[i].name);
    writer.begin_array("bindings");
    for (std::uint32_t b = 0; b < g_mappedActions[i].bindingCount; ++b) {
      const auto &binding = g_mappedActions[i].bindings[b];
      writer.begin_object();
      writer.write_uint("type", static_cast<std::uint32_t>(binding.type));
      writer.write_uint("code", static_cast<std::uint32_t>(binding.code));
      writer.write_float("axis_threshold", binding.axisThreshold);
      writer.write_float("axis_scale", binding.axisScale);
      writer.end_object();
    }
    writer.end_array();
    writer.end_object();
  }
  writer.end_array();

  // Axes array.
  writer.begin_array("axes");
  for (std::size_t i = 0; i < kMaxInputAxes; ++i) {
    if (!g_mappedAxes[i].occupied) {
      continue;
    }
    writer.begin_object();
    writer.write_string("name", g_mappedAxes[i].name);
    writer.begin_array("sources");
    for (std::uint32_t s = 0; s < g_mappedAxes[i].sourceCount; ++s) {
      const auto &src = g_mappedAxes[i].sources[s];
      writer.begin_object();
      writer.write_uint("type", static_cast<std::uint32_t>(src.type));
      writer.write_uint("negative_key",
                        static_cast<std::uint32_t>(src.negativeKey));
      writer.write_uint("positive_key",
                        static_cast<std::uint32_t>(src.positiveKey));
      writer.write_uint("axis_index",
                        static_cast<std::uint32_t>(src.axisIndex));
      writer.write_float("scale", src.scale);
      writer.write_float("dead_zone", src.deadZone);
      writer.end_object();
    }
    writer.end_array();
    writer.end_object();
  }
  writer.end_array();

  writer.end_object();

  if (writer.failed()) {
    log_message(LogLevel::Error, kLogChannel,
                "save_input_bindings: JSON serialization failed");
    return false;
  }

  return write_text_file(path, writer.result(), writer.result_size());
}

bool load_input_bindings(const char *path) noexcept {
  std::size_t fileSize = 0U;
  std::unique_ptr<char[]> fileBuffer{};
  if (!read_text_file(path, &fileBuffer, &fileSize)) {
    log_message(LogLevel::Error, kLogChannel,
                "load_input_bindings: failed to read file");
    return false;
  }
  return load_input_bindings_from_buffer(fileBuffer.get(), fileSize);
}

bool save_input_bindings_to_buffer(char *buffer, std::size_t capacity,
                                   std::size_t *outSize) noexcept {
  if ((buffer == nullptr) || (outSize == nullptr) || (capacity < 2U)) {
    return false;
  }

  JsonWriter writer{};
  writer.begin_object();

  writer.begin_array("actions");
  for (std::size_t i = 0; i < kMaxInputActions; ++i) {
    if (!g_mappedActions[i].occupied) {
      continue;
    }
    writer.begin_object();
    writer.write_string("name", g_mappedActions[i].name);
    writer.begin_array("bindings");
    for (std::uint32_t b = 0; b < g_mappedActions[i].bindingCount; ++b) {
      const auto &binding = g_mappedActions[i].bindings[b];
      writer.begin_object();
      writer.write_uint("type", static_cast<std::uint32_t>(binding.type));
      writer.write_uint("code", static_cast<std::uint32_t>(binding.code));
      writer.write_float("axis_threshold", binding.axisThreshold);
      writer.write_float("axis_scale", binding.axisScale);
      writer.end_object();
    }
    writer.end_array();
    writer.end_object();
  }
  writer.end_array();

  writer.begin_array("axes");
  for (std::size_t i = 0; i < kMaxInputAxes; ++i) {
    if (!g_mappedAxes[i].occupied) {
      continue;
    }
    writer.begin_object();
    writer.write_string("name", g_mappedAxes[i].name);
    writer.begin_array("sources");
    for (std::uint32_t s = 0; s < g_mappedAxes[i].sourceCount; ++s) {
      const auto &src = g_mappedAxes[i].sources[s];
      writer.begin_object();
      writer.write_uint("type", static_cast<std::uint32_t>(src.type));
      writer.write_uint("negative_key",
                        static_cast<std::uint32_t>(src.negativeKey));
      writer.write_uint("positive_key",
                        static_cast<std::uint32_t>(src.positiveKey));
      writer.write_uint("axis_index",
                        static_cast<std::uint32_t>(src.axisIndex));
      writer.write_float("scale", src.scale);
      writer.write_float("dead_zone", src.deadZone);
      writer.end_object();
    }
    writer.end_array();
    writer.end_object();
  }
  writer.end_array();

  writer.end_object();

  if (writer.failed()) {
    return false;
  }

  const std::size_t resultSize = writer.result_size();
  if ((resultSize + 1U) > capacity) {
    return false;
  }

  std::memcpy(buffer, writer.result(), resultSize);
  buffer[resultSize] = '\0';
  *outSize = resultSize;
  return true;
}

bool load_input_bindings_from_buffer(const char *buffer,
                                     std::size_t size) noexcept {
  if ((buffer == nullptr) || (size == 0U)) {
    return false;
  }

  JsonParser parser{};
  if (!parser.parse(buffer, size)) {
    log_message(LogLevel::Error, kLogChannel,
                "load_input_bindings: JSON parse failed");
    return false;
  }

  const JsonValue *root = parser.root();
  if ((root == nullptr) || (root->type != JsonValue::Type::Object)) {
    return false;
  }

  // Clear existing.
  g_mappedActions = {};
  g_mappedAxes = {};
  g_actionDown = {};
  g_prevActionDown = {};

  // Parse actions.
  JsonValue actionsVal{};
  if (parser.get_object_field(*root, "actions", &actionsVal) &&
      (actionsVal.type == JsonValue::Type::Array)) {
    const std::size_t count = parser.array_size(actionsVal);
    for (std::size_t i = 0; i < count && i < kMaxInputActions; ++i) {
      JsonValue actionVal{};
      if (!parser.get_array_element(actionsVal, i, &actionVal) ||
          (actionVal.type != JsonValue::Type::Object)) {
        continue;
      }

      InputAction action{};
      action.occupied = true;

      // Name.
      JsonValue nameVal{};
      if (parser.get_object_field(actionVal, "name", &nameVal)) {
        const char *str = nullptr;
        std::size_t strLen = 0;
        if (parser.as_string(nameVal, &str, &strLen)) {
          if (strLen > kMaxInputNameLen) {
            strLen = kMaxInputNameLen;
          }
          std::memcpy(action.name, str, strLen);
          action.name[strLen] = '\0';
        }
      }

      // Bindings.
      JsonValue bindingsVal{};
      if (parser.get_object_field(actionVal, "bindings", &bindingsVal) &&
          (bindingsVal.type == JsonValue::Type::Array)) {
        const std::size_t bCount = parser.array_size(bindingsVal);
        for (std::size_t b = 0; b < bCount && b < kMaxBindingsPerAction; ++b) {
          JsonValue bVal{};
          if (!parser.get_array_element(bindingsVal, b, &bVal) ||
              (bVal.type != JsonValue::Type::Object)) {
            continue;
          }

          InputBinding binding{};
          std::uint32_t uval = 0;
          float fval = 0.0F;

          JsonValue field{};
          if (parser.get_object_field(bVal, "type", &field) &&
              parser.as_uint(field, &uval)) {
            binding.type = static_cast<InputBindingType>(uval);
          }
          if (parser.get_object_field(bVal, "code", &field) &&
              parser.as_uint(field, &uval)) {
            binding.code = static_cast<int>(uval);
          }
          if (parser.get_object_field(bVal, "axis_threshold", &field) &&
              parser.as_float(field, &fval)) {
            binding.axisThreshold = fval;
          }
          if (parser.get_object_field(bVal, "axis_scale", &field) &&
              parser.as_float(field, &fval)) {
            binding.axisScale = fval;
          }

          action.bindings[action.bindingCount] = binding;
          ++action.bindingCount;
        }
      }

      // Store.
      for (auto &slot : g_mappedActions) {
        if (!slot.occupied) {
          slot = action;
          break;
        }
      }
    }
  }

  // Parse axes.
  JsonValue axesVal{};
  if (parser.get_object_field(*root, "axes", &axesVal) &&
      (axesVal.type == JsonValue::Type::Array)) {
    const std::size_t count = parser.array_size(axesVal);
    for (std::size_t i = 0; i < count && i < kMaxInputAxes; ++i) {
      JsonValue axisVal{};
      if (!parser.get_array_element(axesVal, i, &axisVal) ||
          (axisVal.type != JsonValue::Type::Object)) {
        continue;
      }

      InputAxisMapping axis{};
      axis.occupied = true;

      // Name.
      JsonValue nameVal{};
      if (parser.get_object_field(axisVal, "name", &nameVal)) {
        const char *str = nullptr;
        std::size_t strLen = 0;
        if (parser.as_string(nameVal, &str, &strLen)) {
          if (strLen > kMaxInputNameLen) {
            strLen = kMaxInputNameLen;
          }
          std::memcpy(axis.name, str, strLen);
          axis.name[strLen] = '\0';
        }
      }

      // Sources.
      JsonValue sourcesVal{};
      if (parser.get_object_field(axisVal, "sources", &sourcesVal) &&
          (sourcesVal.type == JsonValue::Type::Array)) {
        const std::size_t sCount = parser.array_size(sourcesVal);
        for (std::size_t s = 0; s < sCount && s < kMaxSourcesPerAxis; ++s) {
          JsonValue sVal{};
          if (!parser.get_array_element(sourcesVal, s, &sVal) ||
              (sVal.type != JsonValue::Type::Object)) {
            continue;
          }

          InputAxisSource src{};
          std::uint32_t uval = 0;
          float fval = 0.0F;

          JsonValue field{};
          if (parser.get_object_field(sVal, "type", &field) &&
              parser.as_uint(field, &uval)) {
            src.type = static_cast<AxisSourceType>(uval);
          }
          if (parser.get_object_field(sVal, "negative_key", &field) &&
              parser.as_uint(field, &uval)) {
            src.negativeKey = static_cast<int>(uval);
          }
          if (parser.get_object_field(sVal, "positive_key", &field) &&
              parser.as_uint(field, &uval)) {
            src.positiveKey = static_cast<int>(uval);
          }
          if (parser.get_object_field(sVal, "axis_index", &field) &&
              parser.as_uint(field, &uval)) {
            src.axisIndex = static_cast<int>(uval);
          }
          if (parser.get_object_field(sVal, "scale", &field) &&
              parser.as_float(field, &fval)) {
            src.scale = fval;
          }
          if (parser.get_object_field(sVal, "dead_zone", &field) &&
              parser.as_float(field, &fval)) {
            src.deadZone = fval;
          }

          axis.sources[axis.sourceCount] = src;
          ++axis.sourceCount;
        }
      }

      // Store.
      for (auto &slot : g_mappedAxes) {
        if (!slot.occupied) {
          slot = axis;
          break;
        }
      }
    }
  }

  return true;
}

} // namespace engine::core
