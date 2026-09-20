// Declares the staging scope a hot-reload chunk runs inside. While it is
// open every externally visible effect of the chunk is either recorded
// (performed now, undone on rollback), held (applied on commit in the
// order the chunk requested it, interleaved with the World writes queued
// in the deferred buffer) or refused before it has any side effect. A
// binding asks the scope first and only then acts, so a scope that cannot
// take one more effect never learns of it after the fact; a scope that
// commits applies every effect it took exactly once and reports any it
// could not apply.
//
// Supported effect set. Recorded: entities created (spawn, spawn_shape,
// clone, instantiate), pool entities acquired, timers set, joints added.
// Held: pool releases, timer cancels, audio calls. Queued: World
// component writes and destroys, through the deferred buffer, which a
// read of the same component sees before commit (read-your-writes);
// entity liveness and name lookup read the committed World. Refused
// while a scope is open: pool creation, joint removal and limits, the
// game mode, persistent game state, player controllers, gravity and the
// camera stack, because none can be undone. Resource loads (sounds,
// streamed assets) run at once and are not undone: they are caches, not
// gameplay state.

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

/// The effects a scope records or holds, asked about one at a time.
enum class ReloadEffect : std::uint8_t {
  CreateEntity,
  PoolAcquire,
  TimerCreate,
  JointCreate,
  PoolRelease,
  TimerCancel,
  Audio,
};

/// What a binding does with one effect right now.
enum class ReloadStaging : std::uint8_t {
  /// No scope is open: the binding performs the effect itself.
  None,
  /// The open scope will take the effect: the binding performs a recorded
  /// effect and notes it, or hands a held effect over instead of
  /// performing it.
  Staged,
  /// The open scope is full for this kind: the binding refuses the effect
  /// before any side effect and reports the refusal to the script.
  Refused,
};

/// Capacity of recorded entity creations in one scope.
constexpr std::size_t kMaxStagedEntities = 256U;
/// Capacity of recorded pool acquisitions, timers and joints, each.
constexpr std::size_t kMaxStagedRecords = 64U;
/// Capacity of held effects of all kinds together, in request order.
constexpr std::size_t kMaxHeldEffects = 128U;

/// Opens the scope; false when one is already open.
bool begin_reload_transaction() noexcept;
/// True while a reload chunk's effects are being staged.
bool reload_transaction_open() noexcept;
/// Asks the scope about one effect before it happens.
ReloadStaging reload_staging(ReloadEffect effect) noexcept;
/// True, with one logged warning naming `what`, when a scope is open: the
/// effect can be neither undone nor held, so the binding refuses it.
bool reload_refuses(const char *what) noexcept;

/// Outcome of a commit.
enum class ReloadCommit : std::uint8_t {
  /// Every staged effect applied exactly once.
  Applied,
  /// The effects applied in order, but at least one reported failure
  /// (logged with the count); what applied stays applied.
  AppliedWithFailures,
  /// The scope could not promise a complete undo (a breach) or no scope
  /// was open; everything recorded has been rolled back.
  Refused,
};

/// Applies the held effects and the queued World writes in the order the
/// chunk requested them, each exactly once, and closes the scope.
ReloadCommit commit_reload_transaction() noexcept;
/// Discards everything staged: queued World writes are dropped, entities
/// the chunk created are destroyed, pool entities it acquired are
/// released, timers it set are cancelled, joints it added are removed,
/// and held effects never happen.
void rollback_reload_transaction() noexcept;

// A note with no scope open is a no-op (the effect simply happened). With
// a scope open, every note or hold requires a preceding reload_staging
// answer of Staged for its effect kind; one without it is a programmer
// error that is logged and makes the commit fail, because the scope can
// no longer promise to undo what it did not record.

/// Records an entity the chunk created (rolled back by destroying it).
void reload_note_created_entity(core::Entity entity) noexcept;
/// Records a pool entity the chunk acquired (rolled back by releasing it).
void reload_note_pool_acquire(std::size_t slot, core::Entity entity) noexcept;
/// Records a timer the chunk set (rolled back by cancelling it).
void reload_note_timer_created(std::uint32_t timerId) noexcept;
/// Records a joint the chunk added (rolled back by removing it).
void reload_note_joint_created(std::uint32_t jointId) noexcept;
/// Holds a pool release until commit.
void reload_hold_pool_release(std::size_t slot, core::Entity entity) noexcept;
/// Holds a timer cancel until commit.
void reload_hold_timer_cancel(std::uint32_t timerId) noexcept;
/// Holds an audio call until commit.
void reload_hold_audio(const StagedAudioOp &op) noexcept;

} // namespace engine::scripting
