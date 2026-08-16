// Implements the registry-generated ComponentEditType capture/apply/presence
// dispatch declared in editor_component_registry.h.

#include "editor_component_registry.h"

#include "editor_session.h"

namespace engine::editor {

bool capture_component_snapshot(ComponentEditType type, runtime::Entity entity,
                                ComponentEditSnapshot *out) noexcept {
  runtime::World *const world = editor_session().world;
  if ((world == nullptr) || (out == nullptr)) {
    return false;
  }

  switch (type) {
#define ENGINE_ICR_CAPTURE_ROW(Type, Key, GetFn, AddFn, RemoveFn)              \
  case ComponentEditType::ENGINE_ICR_ALIAS(Type):                              \
    return world->GetFn(entity, &out->ENGINE_ICR_MEMBER(Type));
    ENGINE_PERSISTENT_COMPONENT_TABLE(ENGINE_ICR_CAPTURE_ROW)
#undef ENGINE_ICR_CAPTURE_ROW
  }
  return false;
}

bool apply_component_snapshot(ComponentEditType type, runtime::Entity entity,
                              bool exists,
                              const ComponentEditSnapshot &snapshot) noexcept {
  runtime::World *const world = editor_session().world;
  if ((world == nullptr) || !world->is_alive(entity)) {
    return false;
  }

  if (!exists) {
    switch (type) {
#define ENGINE_ICR_REMOVE_ROW(Type, Key, GetFn, AddFn, RemoveFn)               \
  case ComponentEditType::ENGINE_ICR_ALIAS(Type):                              \
    return world->RemoveFn(entity);
      ENGINE_PERSISTENT_COMPONENT_TABLE(ENGINE_ICR_REMOVE_ROW)
#undef ENGINE_ICR_REMOVE_ROW
    }
    return false;
  }

  switch (type) {
#define ENGINE_ICR_ADD_ROW(Type, Key, GetFn, AddFn, RemoveFn)                  \
  case ComponentEditType::ENGINE_ICR_ALIAS(Type):                              \
    return world->AddFn(entity, snapshot.ENGINE_ICR_MEMBER(Type));
    ENGINE_PERSISTENT_COMPONENT_TABLE(ENGINE_ICR_ADD_ROW)
#undef ENGINE_ICR_ADD_ROW
  }
  return false;
}

bool has_component_of_type(ComponentEditType type,
                           runtime::Entity entity) noexcept {
  ComponentEditSnapshot scratch{};
  return capture_component_snapshot(type, entity, &scratch);
}

} // namespace engine::editor
