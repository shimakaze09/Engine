// Lets the action mapper evaluate a binding against the step before the
// current fixed step, which is how an action's press is found per step.

#pragma once

namespace engine::core {

/// True while a fixed step's snapshot answers input queries.
bool input_step_current() noexcept;
/// Points input queries at the previous step's snapshot (true) or back at
/// the current one (false). No effect when no step is current.
void input_step_use_previous(bool previous) noexcept;

} // namespace engine::core
