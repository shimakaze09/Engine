// Declares the staging scope a hot-reload chunk runs inside. While it is
// open every externally visible effect of the chunk is held back: World
// component writes queue in the deferred buffer, created entities and
// acquired pool entities are recorded, timers are recorded and cancels
// held, and audio calls are buffered. The scope commits when the chunk
// returns cleanly within budget and rolls back otherwise, so a failed
// reload leaves timer, audio and entity state as it found them.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/entity.h"

namespace engine::scripting {

/// One audio call held until the reload commits.
struct StagedAudioOp final {
  enum class Kind : std::uint8_t {
    UnloadSound,
    PlaySound,
    StopSound,
    StopAllSounds,
    SetMasterVolume,
    PlaySoundAt,
    SetBusVolume,
    PlayMusic,
    StopMusic,
  };
  static constexpr std::size_t kMaxPathLength = 255U;

  Kind kind = Kind::StopAllSounds;
  std::uint32_t id = 0U;
  float a = 0.0F;
  float b = 0.0F;
  float c = 0.0F;
  float d = 0.0F;
  bool flag = false;
  char path[kMaxPathLength + 1U] = {};
};

/// Opens the scope; false when one is already open.
bool begin_reload_transaction() noexcept;
/// True while a reload chunk's effects are being staged.
bool reload_transaction_open() noexcept;
/// Applies everything staged, in the order the chunk requested it, and
/// flushes the deferred World writes the chunk queued.
void commit_reload_transaction() noexcept;
/// Discards everything staged: queued World writes are dropped, entities
/// the chunk created are destroyed, pool entities it acquired are
/// released, timers it set are cancelled, and held audio calls, timer
/// cancels and pool releases never happen.
void rollback_reload_transaction() noexcept;

/// Records an entity the chunk created (rolled back by destroying it).
void reload_note_created_entity(core::Entity entity) noexcept;
/// Records a pool entity the chunk acquired (rolled back by releasing it).
void reload_note_pool_acquire(std::size_t slot, core::Entity entity) noexcept;
/// Holds a pool release until commit; false when no scope is open, so the
/// caller performs the release itself.
bool reload_stage_pool_release(std::size_t slot, core::Entity entity) noexcept;
/// Records a timer the chunk set (rolled back by cancelling it).
void reload_note_timer_created(std::uint32_t timerId) noexcept;
/// Holds a timer cancel until commit; false when no scope is open.
bool reload_stage_timer_cancel(std::uint32_t timerId) noexcept;
/// Holds an audio call until commit; false when no scope is open or the
/// staging buffer is full (the caller then performs or refuses the call).
bool reload_stage_audio(const StagedAudioOp &op) noexcept;

} // namespace engine::scripting
