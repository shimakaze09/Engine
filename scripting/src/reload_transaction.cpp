// Implements the hot-reload staging scope. One scope exists at a time
// (reloads never nest); every record lives in fixed storage so staging
// never allocates, and a record buffer that fills logs the loss because a
// rollback can then only undo what it recorded.

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

constexpr std::size_t kMaxStagedEntities = 256U;
constexpr std::size_t kMaxStagedPoolOps = 64U;
constexpr std::size_t kMaxStagedTimers = 64U;
constexpr std::size_t kMaxStagedAudioOps = 64U;

/// One pool acquisition or release the chunk performed.
struct StagedPoolOp final {
  std::size_t slot = 0U;
  core::Entity entity = core::kInvalidEntity;
};

struct ReloadTransaction final {
  bool open = false;
  std::size_t deferredCountAtBegin = 0U;
  core::Entity created[kMaxStagedEntities]{};
  std::size_t createdCount = 0U;
  StagedPoolOp acquired[kMaxStagedPoolOps]{};
  std::size_t acquiredCount = 0U;
  StagedPoolOp releases[kMaxStagedPoolOps]{};
  std::size_t releaseCount = 0U;
  std::uint32_t timersCreated[kMaxStagedTimers]{};
  std::size_t timersCreatedCount = 0U;
  std::uint32_t timerCancels[kMaxStagedTimers]{};
  std::size_t timerCancelCount = 0U;
  StagedAudioOp audio[kMaxStagedAudioOps]{};
  std::size_t audioCount = 0U;
  std::size_t unrecorded = 0U;
};

ReloadTransaction g_transaction{};

void log_overflow(const char *what) noexcept {
  char message[128] = {};
  std::snprintf(message, sizeof(message),
                "hot_reload: %s buffer full; a failed reload cannot undo it",
                what);
  core::log_message(core::LogLevel::Error, "scripting", message);
  ++g_transaction.unrecorded;
}

/// Replays one held audio call through the bound services.
void replay_audio(const StagedAudioOp &op) noexcept {
  const RuntimeServices *services = runtime_binding().services;
  if (services == nullptr) {
    return;
  }
  using Kind = StagedAudioOp::Kind;
  switch (op.kind) {
  case Kind::UnloadSound:
    if (services->unload_sound != nullptr) {
      services->unload_sound(op.id);
    }
    break;
  case Kind::PlaySound:
    if (services->play_sound != nullptr) {
      static_cast<void>(services->play_sound(op.id, op.a, op.b, op.flag));
    }
    break;
  case Kind::StopSound:
    if (services->stop_sound != nullptr) {
      services->stop_sound(op.id);
    }
    break;
  case Kind::StopAllSounds:
    if (services->stop_all_sounds != nullptr) {
      services->stop_all_sounds();
    }
    break;
  case Kind::SetMasterVolume:
    if (services->set_master_volume != nullptr) {
      services->set_master_volume(op.a);
    }
    break;
  case Kind::PlaySoundAt:
    if (services->play_sound_at != nullptr) {
      static_cast<void>(
          services->play_sound_at(op.id, op.a, op.b, op.c, op.d));
    }
    break;
  case Kind::SetBusVolume:
    if (services->set_bus_volume != nullptr) {
      services->set_bus_volume(op.id, op.a);
    }
    break;
  case Kind::PlayMusic:
    if (services->play_music != nullptr) {
      static_cast<void>(services->play_music(op.path, op.a, op.flag));
    }
    break;
  case Kind::StopMusic:
    if (services->stop_music != nullptr) {
      services->stop_music();
    }
    break;
  }
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

void commit_reload_transaction() noexcept {
  if (!g_transaction.open) {
    return;
  }
  // Closing the scope first lets the deferred flush and the held calls
  // apply immediately instead of being staged again.
  g_transaction.open = false;
  for (std::size_t i = 0U; i < g_transaction.audioCount; ++i) {
    replay_audio(g_transaction.audio[i]);
  }
  for (std::size_t i = 0U; i < g_transaction.timerCancelCount; ++i) {
    cancel_lua_timer(g_transaction.timerCancels[i]);
  }
  const ScriptingRuntimeBinding &binding = runtime_binding();
  if (runtime_bound() && (binding.services->entity_pool_release != nullptr)) {
    for (std::size_t i = 0U; i < g_transaction.releaseCount; ++i) {
      const StagedPoolOp &op = g_transaction.releases[i];
      static_cast<void>(
          binding.services->entity_pool_release(binding.world, op.slot, op.entity));
    }
  }
  flush_deferred_mutations();
  reset_transaction();
}

void rollback_reload_transaction() noexcept {
  if (!g_transaction.open) {
    return;
  }
  g_transaction.open = false;
  truncate_deferred_mutations(g_transaction.deferredCountAtBegin);
  const ScriptingRuntimeBinding &binding = runtime_binding();
  if (runtime_bound()) {
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
  if (g_transaction.unrecorded > 0U) {
    char message[128] = {};
    std::snprintf(message, sizeof(message),
                  "hot_reload: rollback left %zu unrecorded effect(s) in place",
                  g_transaction.unrecorded);
    core::log_message(core::LogLevel::Error, "scripting", message);
  }
  reset_transaction();
}

void reload_note_created_entity(core::Entity entity) noexcept {
  if (!g_transaction.open || (entity == core::kInvalidEntity)) {
    return;
  }
  if (g_transaction.createdCount >= kMaxStagedEntities) {
    log_overflow("created entity");
    return;
  }
  g_transaction.created[g_transaction.createdCount] = entity;
  ++g_transaction.createdCount;
}

void reload_note_pool_acquire(std::size_t slot, core::Entity entity) noexcept {
  if (!g_transaction.open || (entity == core::kInvalidEntity)) {
    return;
  }
  if (g_transaction.acquiredCount >= kMaxStagedPoolOps) {
    log_overflow("pool acquire");
    return;
  }
  g_transaction.acquired[g_transaction.acquiredCount] = {slot, entity};
  ++g_transaction.acquiredCount;
}

bool reload_stage_pool_release(std::size_t slot, core::Entity entity) noexcept {
  if (!g_transaction.open) {
    return false;
  }
  if (g_transaction.releaseCount >= kMaxStagedPoolOps) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "hot_reload: pool release buffer full; release refused");
    return true;
  }
  g_transaction.releases[g_transaction.releaseCount] = {slot, entity};
  ++g_transaction.releaseCount;
  return true;
}

void reload_note_timer_created(std::uint32_t timerId) noexcept {
  if (!g_transaction.open || (timerId == 0U)) {
    return;
  }
  if (g_transaction.timersCreatedCount >= kMaxStagedTimers) {
    log_overflow("created timer");
    return;
  }
  g_transaction.timersCreated[g_transaction.timersCreatedCount] = timerId;
  ++g_transaction.timersCreatedCount;
}

bool reload_stage_timer_cancel(std::uint32_t timerId) noexcept {
  if (!g_transaction.open) {
    return false;
  }
  if (g_transaction.timerCancelCount >= kMaxStagedTimers) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "hot_reload: timer cancel buffer full; cancel refused");
    return true;
  }
  g_transaction.timerCancels[g_transaction.timerCancelCount] = timerId;
  ++g_transaction.timerCancelCount;
  return true;
}

bool reload_stage_audio(const StagedAudioOp &op) noexcept {
  if (!g_transaction.open) {
    return false;
  }
  if (g_transaction.audioCount >= kMaxStagedAudioOps) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "hot_reload: audio buffer full; call refused");
    return false;
  }
  g_transaction.audio[g_transaction.audioCount] = op;
  ++g_transaction.audioCount;
  return true;
}

} // namespace engine::scripting
