// The native and scripted file-dialog slot table: a fixed set of slots that
// carry a dialog from request through the answering thread's result to the
// requester taking it, and the platform_*file_dialog* entry points that drive
// them. Native dialogs go through SDL; a scripted mode answers from a test
// instead of showing anything.

#include "engine/core/platform.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include <SDL3/SDL.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

#include "engine/core/logging.h"
#include "engine/core/thread_affinity.h"
#include "platform_internal.h"

namespace engine::core {

using platform_detail::g_window;
using platform_detail::log_sdl_error;

namespace {

/// Where a dialog slot is in its life. The main thread moves a slot out
/// of Free (claim) and out of Delivered (take); the thread that answers
/// the dialog moves it out of Pending and Abandoned. Every transition is
/// on `state`, so it is the slot's only cross-thread contract.
enum class DialogSlotState : std::uint8_t {
  Free,
  /// Shown and not yet answered.
  Pending,
  /// Given up while still open; the answer frees the slot.
  Abandoned,
  /// Answered; the result waits for the requester to take it.
  Delivered,
};

/// One dialog, from request to taken result. SDL reads the filter array
/// after the show call returns, so a copy lives here until the dialog
/// closes. The answering thread writes outcome and path, then publishes
/// them with a release on state; the main thread reads them only after
/// an acquire load sees Delivered. The main thread reuses a slot only
/// once it is Free again, and an answer's last access to the slot is the
/// store that frees or delivers it.
struct DialogSlot final {
  std::atomic<DialogSlotState> state{DialogSlotState::Free};
  /// Set by the main thread after the claim, before the dialog is shown,
  /// and kept until the next claim; state says whether it is still live.
  /// Atomic because a scripted answer may look it up from another thread.
  std::atomic<FileDialogTicket> ticket{kNoFileDialog};
  bool scripted = false;
  FileDialogOutcome outcome = FileDialogOutcome::Cancelled;
  std::array<char, kMaxFileDialogPathLength> path{};
  std::array<SDL_DialogFileFilter,
             static_cast<std::size_t>(kMaxFileDialogFilters)>
      filters{};
};
constexpr std::size_t kDialogSlotCount =
    static_cast<std::size_t>(kMaxPendingFileDialogs);
std::array<DialogSlot, kDialogSlotCount> g_dialogSlots{};
// Main thread only. Counts claims; a ticket encodes it with the slot index,
// so a slot's successive tickets never repeat.
std::uint32_t g_dialogClaims = 0U;
bool g_scriptedDialogs = false;

/// The slot a ticket was issued for, or null for kNoFileDialog.
DialogSlot *dialog_slot_for(FileDialogTicket ticket) noexcept {
  if (ticket == kNoFileDialog) {
    return nullptr;
  }
  return &g_dialogSlots[static_cast<std::size_t>(ticket - 1U) %
                        kDialogSlotCount];
}

/// Records an answer and hands it to whoever holds the ticket, or frees
/// the slot when nobody does any more. Runs on the answering thread, once
/// per shown dialog.
void deliver_dialog_answer(DialogSlot &slot, FileDialogOutcome outcome,
                           const char *path) noexcept {
  slot.path[0] = '\0';
  if (outcome == FileDialogOutcome::Chosen) {
    // Delivered with '/' separators, as the platform's other paths are, so
    // a chosen file compares equal to the same file reached any other way
    // (Windows dialogs answer with '\').
    if (!platform_detail::copy_normalized_path(path, slot.path.data(),
                                               slot.path.size())) {
      outcome = FileDialogOutcome::PathTooLong;
      log_message(LogLevel::Error, "platform",
                  "the chosen path is longer than a file dialog result can "
                  "hold; it was refused");
    }
  }
  slot.outcome = outcome;
  DialogSlotState expected = DialogSlotState::Pending;
  if (!slot.state.compare_exchange_strong(expected, DialogSlotState::Delivered,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
    // Abandoned while open: nobody will take this answer.
    slot.state.store(DialogSlotState::Free, std::memory_order_release);
  }
}

/// SDL's callback, translated to the engine's outcomes: a null list is a
/// failure, an empty list a cancel.
void SDLCALL dialog_trampoline(void *userdata, const char *const *filelist,
                               int /*filter*/) noexcept {
  auto *slot = static_cast<DialogSlot *>(userdata);
  if (slot == nullptr) {
    return;
  }
  if (filelist == nullptr) {
    log_sdl_error("native file dialog failed");
    deliver_dialog_answer(*slot, FileDialogOutcome::Failed, nullptr);
  } else if (filelist[0] == nullptr) {
    deliver_dialog_answer(*slot, FileDialogOutcome::Cancelled, nullptr);
  } else {
    deliver_dialog_answer(*slot, FileDialogOutcome::Chosen, filelist[0]);
  }
}

} // namespace

FileDialogTicket
platform_request_file_dialog(FileDialogKind kind,
                             const FileDialogFilter *filters, int filterCount,
                             const char *defaultLocation) noexcept {
  ENGINE_ASSERT_MAIN_THREAD();
  if (!g_scriptedDialogs && (g_window == nullptr)) {
    log_message(LogLevel::Warning, "platform",
                "native file dialog refused: no window to parent it");
    return kNoFileDialog;
  }
  if ((filterCount < 0) || (filterCount > kMaxFileDialogFilters) ||
      ((filterCount > 0) && (filters == nullptr)) ||
      ((kind == FileDialogKind::Folder) && (filterCount != 0))) {
    log_message(LogLevel::Warning, "platform",
                "native file dialog refused: bad filter list");
    return kNoFileDialog;
  }

  std::size_t index = kDialogSlotCount;
  for (std::size_t i = 0U; i < kDialogSlotCount; ++i) {
    DialogSlotState expected = DialogSlotState::Free;
    if (g_dialogSlots[i].state.compare_exchange_strong(
            expected, DialogSlotState::Pending, std::memory_order_acquire)) {
      index = i;
      break;
    }
  }
  if (index == kDialogSlotCount) {
    log_message(LogLevel::Warning, "platform",
                "native file dialog refused: every dialog slot is held by "
                "a dialog that has not closed");
    return kNoFileDialog;
  }

  DialogSlot &slot = g_dialogSlots[index];
  // Unsigned wraparound keeps (ticket - 1) % kDialogSlotCount == index,
  // since the slot count divides 2^32; only zero is skipped.
  FileDialogTicket ticket = kNoFileDialog;
  while (ticket == kNoFileDialog) {
    ++g_dialogClaims;
    ticket = g_dialogClaims * static_cast<FileDialogTicket>(kDialogSlotCount) +
             static_cast<FileDialogTicket>(index) + 1U;
  }
  slot.scripted = g_scriptedDialogs;
  slot.ticket.store(ticket, std::memory_order_release);
  if (slot.scripted) {
    return ticket;
  }

  for (int i = 0; i < filterCount; ++i) {
    slot.filters[static_cast<std::size_t>(i)] =
        SDL_DialogFileFilter{filters[i].name, filters[i].pattern};
  }
  const SDL_DialogFileFilter *sdlFilters =
      (filterCount > 0) ? slot.filters.data() : nullptr;
  if (kind == FileDialogKind::Save) {
    SDL_ShowSaveFileDialog(&dialog_trampoline, &slot, g_window, sdlFilters,
                           filterCount, defaultLocation);
  } else if (kind == FileDialogKind::Folder) {
    SDL_ShowOpenFolderDialog(&dialog_trampoline, &slot, g_window,
                             defaultLocation, false);
  } else {
    SDL_ShowOpenFileDialog(&dialog_trampoline, &slot, g_window, sdlFilters,
                           filterCount, defaultLocation, false);
  }
  return ticket;
}

FileDialogPoll
platform_take_file_dialog_result(FileDialogTicket ticket,
                                 FileDialogResult *out) noexcept {
  ENGINE_ASSERT_MAIN_THREAD();
  DialogSlot *slot = dialog_slot_for(ticket);
  if ((slot == nullptr) ||
      (slot->ticket.load(std::memory_order_relaxed) != ticket)) {
    return FileDialogPoll::Unknown;
  }
  switch (slot->state.load(std::memory_order_acquire)) {
  case DialogSlotState::Pending:
    return FileDialogPoll::Pending;
  case DialogSlotState::Delivered:
    break;
  case DialogSlotState::Free:
  case DialogSlotState::Abandoned:
  default:
    return FileDialogPoll::Unknown;
  }
  if (out != nullptr) {
    out->ticket = ticket;
    out->outcome = slot->outcome;
    std::memcpy(out->path, slot->path.data(), sizeof(out->path));
  }
  slot->state.store(DialogSlotState::Free, std::memory_order_release);
  return FileDialogPoll::Ready;
}

void platform_abandon_file_dialog(FileDialogTicket ticket) noexcept {
  ENGINE_ASSERT_MAIN_THREAD();
  DialogSlot *slot = dialog_slot_for(ticket);
  if ((slot == nullptr) ||
      (slot->ticket.load(std::memory_order_relaxed) != ticket)) {
    return;
  }
  DialogSlotState expected = DialogSlotState::Pending;
  if (slot->state.compare_exchange_strong(expected, DialogSlotState::Abandoned,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
    return; // the answer, when it comes, frees the slot
  }
  if (expected == DialogSlotState::Delivered) {
    slot->state.store(DialogSlotState::Free, std::memory_order_release);
  }
}

void platform_set_scripted_file_dialogs(bool enabled) noexcept {
  ENGINE_ASSERT_MAIN_THREAD();
  g_scriptedDialogs = enabled;
}

bool platform_answer_scripted_file_dialog(FileDialogTicket ticket,
                                          const char *path) noexcept {
  DialogSlot *slot = dialog_slot_for(ticket);
  // The acquire on ticket pairs with the release that published it, so
  // `scripted` is read as the request wrote it. An abandoned request still
  // waits for its answer to free the slot, exactly as an open native
  // dialog does.
  if ((slot == nullptr) ||
      (slot->ticket.load(std::memory_order_acquire) != ticket) ||
      !slot->scripted) {
    return false;
  }
  const DialogSlotState state = slot->state.load(std::memory_order_acquire);
  if ((state != DialogSlotState::Pending) &&
      (state != DialogSlotState::Abandoned)) {
    return false;
  }
  deliver_dialog_answer(*slot,
                        (path != nullptr) ? FileDialogOutcome::Chosen
                                          : FileDialogOutcome::Cancelled,
                        path);
  return true;
}

} // namespace engine::core
