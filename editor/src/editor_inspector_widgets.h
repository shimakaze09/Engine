// Declares the Inspector's numeric widgets: the ImGui text, drag and
// slider fields wrapped so a typed value reaches the component once,
// when the edit ends. ImGui writes a text field's parsed value on every
// keystroke, and a drag or slider does the same while its text-input
// mode (double-click, Ctrl+click, keyboard focus) is open, so "0.3"
// would reach the World as 0, then 0., then 0.3: two mutations the
// author never meant, and a refusal logged every frame for the ones a
// component rejects. These widgets keep the in-progress value in a draft
// while the field is being typed into and hand it over on Enter, Tab or
// a click elsewhere; Escape discards it. A drag scrubbed with the mouse
// still applies continuously. Each returns true on the frame the value
// changed, the way the ImGui call it wraps does for a scrub.

#pragma once

#include <cstdint>

namespace engine::editor {

/// InputFloat that reports and writes only when the edit ends.
bool inspector_input_float(const char *label, float *value) noexcept;
/// InputScalar for a signed 32-bit integer, committed when the edit ends.
bool inspector_input_int(const char *label, std::int32_t *value) noexcept;
/// InputScalar for an unsigned 32-bit integer, committed when the edit
/// ends.
bool inspector_input_uint(const char *label, std::uint32_t *value) noexcept;

/// DragFloat whose text-input mode commits when the edit ends; a scrub
/// applies live. `min == max` leaves the range open, as ImGui does.
bool inspector_drag_float(const char *label, float *value, float speed,
                          float min, float max,
                          const char *format = "%.3f") noexcept;
/// SliderFloat whose text-input mode commits when the edit ends.
bool inspector_slider_float(const char *label, float *value, float min,
                            float max, const char *format = "%.3f") noexcept;
/// Three DragFloats sharing one item width, each committing its own typed
/// value when its edit ends; `values` points at three floats.
bool inspector_drag_float3(const char *label, float *values, float speed,
                           float min, float max,
                           const char *format = "%.3f") noexcept;
/// Three SliderFloats sharing one item width, committed like
/// inspector_drag_float3.
bool inspector_slider_float3(const char *label, float *values, float min,
                             float max,
                             const char *format = "%.3f") noexcept;

} // namespace engine::editor
