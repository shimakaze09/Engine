// Implements the hot-reload staging scope. One scope exists at a time
// (reloads never nest); every record lives in fixed storage so staging
// never allocates. Recorded effects keep one undo table per kind; held
// effects share one journal in request order, each entry remembering how
// many World writes were queued before it so the commit can interleave
// the deferred flush with the held calls exactly as the chunk asked.

#include "reload_transaction.h"

#include <cstdio>

#include "deferred_mutations.h"
#include "engine/core/logging.h"
#include "engine/scripting/runtime_services.h"
#include "engine/scripting/scripting.h"
#include "runtime_binding.h"
#include "timer_bindings.h"

namespace engine::scripting {
namespace {

/// One pool acquisition or release the chunk performed.
struct StagedPoolOp final {
  std::size_t slot = 0U;
  core::Entity entity = core::kInvalidEntity;
};

/// One effect held until commit, with its place among the queued writes.
struct HeldEffect final {
  enum class Kind : std::uint8_t { PoolRelease, TimerCancel, Audio };
  Kind kind = Kind::Audio;
  std::size_t deferredCountBefore = 0U;
  StagedPoolOp pool{};
  std::uint32_t timerId = 0U;
  StagedAudioOp audio{};
};

struct ReloadTransaction final {
  bool open = false;
  /// Set when a note or hold arrived that the scope did not agree to take;
  /// the scope can then no longer promise a complete undo, so it commits
  /// nothing.
  bool breached = false;
  std::size_t deferredCountAtBegin = 0U;
  core::Entity created[kMaxStagedEntities]{};
  std::size_t createdCount = 0U;
  StagedPoolOp acquired[kMaxStagedRecords]{};
  std::size_t acquiredCount = 0U;
  std::uint32_t timersCreated[kMaxStagedRecords]{};
  std::size_t timersCreatedCount = 0U;
  std::uint32_t jointsCreated[kMaxStagedRecords]{};
  std::size_t jointsCreatedCount = 0U;
  HeldEffect held[kMaxHeldEffects]{};
  std::size_t heldCount = 0U;
};

ReloadTransaction g_transaction{};

void log_breach(const char *what) noexcept {
  char message[160] = {};
  std::snprintf(message, sizeof(message),
                "hot_reload: %s was not reserved before it happened; the "
                "reload will not commit",
                what);
  core::log_message(core::LogLevel::Error, "scripting", message);
  g_transaction.breached = true;
}

/// Answers Staged when `count` leaves room below `capacity`.
ReloadStaging room(std::size_t count, std::size_t capacity) noexcept {
  return (count < capacity) ? ReloadStaging::Staged : ReloadStaging::Refused;
}

/// Replays one held audio call through the bound services; false when the
/// call reports failure.
bool replay_audio(const StagedAudioOp &op) noexcept {
  const RuntimeServices *services = runtime_binding().services;
  if (services == nullptr) {
    return false;
  }
  using Kind = StagedAudioOp::Kind;
  switch (op.kind) {
  case Kind::UnloadSound:
    if (services->unload_sound != nullptr) {
      services->unload_sound(op.id);
    }
    return true;
  case Kind::PlaySound:
    return (services->play_sound != nullptr) &&
           services->play_sound(op.id, op.a, op.b, op.flag);
  case Kind::StopSound:
    if (services->stop_sound != nullptr) {
      services->stop_sound(op.id);
    }
    return true;
  case Kind::StopAllSounds:
    if (services->stop_all_sounds != nullptr) {
      services->stop_all_sounds();
    }
    return true;
  case Kind::SetMasterVolume:
    if (services->set_master_volume != nullptr) {
      services->set_master_volume(op.a);
    }
    return true;
  case Kind::PlaySoundAt:
    return (services->play_sound_at != nullptr) &&
           services->play_sound_at(op.id, op.a, op.b, op.c, op.d);
  case Kind::SetBusVolume:
    if (services->set_bus_volume != nullptr) {
      services->set_bus_volume(op.id, op.a);
    }
    return true;
  case Kind::PlayMusic:
    return (services->play_music != nullptr) &&
           services->play_music(op.path, op.a, op.flag);
  case Kind::StopMusic:
    if (services->stop_music != nullptr) {
      services->stop_music();
    }
    return true;
  }
  return false;
}

/// Applies one held effect; false when it reports failure.
bool apply_held(const HeldEffect &effect) noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  switch (effect.kind) {
  case HeldEffect::Kind::PoolRelease:
    return runtime_bound() &&
           (binding.services->entity_pool_release != nullptr) &&
           binding.services->entity_pool_release(
               binding.world, effect.pool.slot, effect.pool.entity);
  case HeldEffect::Kind::TimerCancel:
    cancel_lua_timer(effect.timerId);
    return true;
  case HeldEffect::Kind::Audio:
    return replay_audio(effect.audio);
  }
  return false;
}

/// Appends one held effect; the caller has reserved the slot.
void hold(const HeldEffect &effect) noexcept {
  if (!g_transaction.open) {
    log_breach("a held effect");
    return;
  }
  if (g_transaction.heldCount >= kMaxHeldEffects) {
    log_breach("a held effect");
    return;
  }
  g_transaction.held[g_transaction.heldCount] = effect;
  g_transaction.held[g_transaction.heldCount].deferredCountBefore =
      deferred_mutation_count();
  ++g_transaction.heldCount;
}

void reset_transaction() noexcept { g_transaction = ReloadTransaction{}; }

} // namespace

bool begin_reload_transaction() noexcept {
  if (g_transaction.open) {
    return false;
  }
  reset_transaction();
  g_transaction.deferredCountAtBegin = deferred_mutation_count();
  g_transaction.open = true;
  return true;
}

bool reload_transaction_open() noexcept { return g_transaction.open; }

ReloadStaging reload_staging(ReloadEffect effect) noexcept {
  if (!g_transaction.open) {
    return ReloadStaging::None;
  }
  switch (effect) {
  case ReloadEffect::CreateEntity:
    return room(g_transaction.createdCount, kMaxStagedEntities);
  case ReloadEffect::PoolAcquire:
    return room(g_transaction.acquiredCount, kMaxStagedRecords);
  case ReloadEffect::TimerCreate:
    return room(g_transaction.timersCreatedCount, kMaxStagedRecords);
  case ReloadEffect::JointCreate:
    return room(g_transaction.jointsCreatedCount, kMaxStagedRecords);
  case ReloadEffect::PoolRelease:
  case ReloadEffect::TimerCancel:
  case ReloadEffect::Audio:
    return room(g_transaction.heldCount, kMaxHeldEffects);
  }
  return ReloadStaging::Refused;
}

bool reload_refuses(const char *what) noexcept {
  if (!g_transaction.open) {
    return false;
  }
  char message[160] = {};
  std::snprintf(message, sizeof(message),
                "hot_reload: %s is refused while a script hot reload runs; "
                "it cannot be undone",
                (what != nullptr) ? what : "this call");
  core::log_message(core::LogLevel::Warning, "scripting", message);
  return true;
}

ReloadCommit commit_reload_transaction() noexcept {
  if (!g_transaction.open) {
    return ReloadCommit::Refused;
  }
  if (g_transaction.breached) {
    rollback_reload_transaction();
    return ReloadCommit::Refused;
  }
  // Closing the scope first lets the deferred flush and the held calls
  // apply immediately instead of being staged again.
  g_transaction.open = false;
  std::size_t failures = 0U;
  std::size_t flushed = 0U;
  for (std::size_t i = 0U; i < g_transaction.heldCount; ++i) {
    const HeldEffect &effect = g_transaction.held[i];
    // The writes queued before this effect apply before it; the queue
    // compacts as it drains, so the prefix is measured from what already
    // drained.
    const std::size_t prefix = (effect.deferredCountBefore > flushed)
                                   ? effect.deferredCountBefore - flushed
                                   : 0U;
    failures += flush_deferred_mutations_prefix(prefix);
    flushed += prefix;
    if (!apply_held(effect)) {
      ++failures;
    }
  }
  failures += flush_deferred_mutations_prefix(deferred_mutation_count());
  if (failures > 0U) {
    char message[128] = {};
    std::snprintf(message, sizeof(message),
                  "hot_reload: committed with %zu effect(s) that failed to "
                  "apply",
                  failures);
    core::log_message(core::LogLevel::Error, "scripting", message);
  }
  reset_transaction();
  return (failures == 0U) ? ReloadCommit::Applied
                          : ReloadCommit::AppliedWithFailures;
}

void rollback_reload_transaction() noexcept {
  if (!g_transaction.open) {
    return;
  }
  g_transaction.open = false;
  truncate_deferred_mutations(g_transaction.deferredCountAtBegin);
  const ScriptingRuntimeBinding &binding = runtime_binding();
  if (runtime_bound()) {
    if (binding.services->remove_joint != nullptr) {
      for (std::size_t i = g_transaction.jointsCreatedCount; i > 0U; --i) {
        static_cast<void>(binding.services->remove_joint(
            binding.world, g_transaction.jointsCreated[i - 1U]));
      }
    }
    for (std::size_t i = g_transaction.createdCount; i > 0U; --i) {
      static_cast<void>(binding.services->destroy_entity_op(
          binding.world, g_transaction.created[i - 1U]));
    }
    if (binding.services->entity_pool_release != nullptr) {
      for (std::size_t i = g_transaction.acquiredCount; i > 0U; --i) {
        const StagedPoolOp &op = g_transaction.acquired[i - 1U];
        static_cast<void>(binding.services->entity_pool_release(
            binding.world, op.slot, op.entity));
      }
    }
  }
  for (std::size_t i = g_transaction.timersCreatedCount; i > 0U; --i) {
    cancel_lua_timer(g_transaction.timersCreated[i - 1U]);
  }
  reset_transaction();
}

void reload_note_created_entity(core::Entity entity) noexcept {
  if (entity == core::kInvalidEntity) {
    return;
  }
  if (!g_transaction.open) {
    return;
  }
  if (g_transaction.createdCount >= kMaxStagedEntities) {
    log_breach("a created entity");
    return;
  }
  g_transaction.created[g_transaction.createdCount] = entity;
  ++g_transaction.createdCount;
}

void reload_note_pool_acquire(std::size_t slot, core::Entity entity) noexcept {
  if (entity == core::kInvalidEntity) {
    return;
  }
  if (!g_transaction.open) {
    return;
  }
  if (g_transaction.acquiredCount >= kMaxStagedRecords) {
    log_breach("a pool acquire");
    return;
  }
  g_transaction.acquired[g_transaction.acquiredCount] = {slot, entity};
  ++g_transaction.acquiredCount;
}

void reload_note_timer_created(std::uint32_t timerId) noexcept {
  if (timerId == 0U) {
    return;
  }
  if (!g_transaction.open) {
    return;
  }
  if (g_transaction.timersCreatedCount >= kMaxStagedRecords) {
    log_breach("a created timer");
    return;
  }
  g_transaction.timersCreated[g_transaction.timersCreatedCount] = timerId;
  ++g_transaction.timersCreatedCount;
}

void reload_note_joint_created(std::uint32_t jointId) noexcept {
  if (jointId == 0U) {
    return;
  }
  if (!g_transaction.open) {
    return;
  }
  if (g_transaction.jointsCreatedCount >= kMaxStagedRecords) {
    log_breach("a created joint");
    return;
  }
  g_transaction.jointsCreated[g_transaction.jointsCreatedCount] = jointId;
  ++g_transaction.jointsCreatedCount;
}

void reload_hold_pool_release(std::size_t slot, core::Entity entity) noexcept {
  HeldEffect effect{};
  effect.kind = HeldEffect::Kind::PoolRelease;
  effect.pool = {slot, entity};
  hold(effect);
}

void reload_hold_timer_cancel(std::uint32_t timerId) noexcept {
  HeldEffect effect{};
  effect.kind = HeldEffect::Kind::TimerCancel;
  effect.timerId = timerId;
  hold(effect);
}

void reload_hold_audio(const StagedAudioOp &op) noexcept {
  HeldEffect effect{};
  effect.kind = HeldEffect::Kind::Audio;
  effect.audio = op;
  hold(effect);
}

} // namespace engine::scripting
