// Declares the runtime side of the scripting bridge: installing the
// RuntimeServices table scripting declares and the World into scripting's
// service locators.

#pragma once

#include "engine/scripting/runtime_services.h"

namespace engine::runtime {

/// Binds scripting runtime pointers into an explicit service locator.
void bind_scripting_runtime(World *world,
                            core::ServiceLocator &locator) noexcept;
/// Clears scripting runtime bindings from an explicit service locator.
void unbind_scripting_runtime(core::ServiceLocator &locator) noexcept;

} // namespace engine::runtime
