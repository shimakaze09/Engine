// Implements editor bridge behavior for the Engine runtime world.

#include "engine/runtime/editor_bridge.h"

namespace engine::runtime {

namespace {

const EditorBridge *g_editorBridge = nullptr;

} // namespace

/// Sets the requested value for editor bridge.
void set_editor_bridge(const EditorBridge *bridge) noexcept {
  g_editorBridge = bridge;
}

const EditorBridge *editor_bridge() noexcept {
  return g_editorBridge;
}

} // namespace engine::runtime
