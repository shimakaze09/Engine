// Implements NativeThread over _beginthreadex/pthread_create so worker
// spawning reports OS resource failure as a return value; std::thread's
// throwing constructor would terminate the no-exception build instead
// (audit H-14).

#include "engine/core/native_thread.h"

#include <cstring>
#include <new>

#ifdef _WIN32
#include <process.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace engine::core {

namespace {

/// Heap context carrying the entry across the OS trampoline; freed by
/// the trampoline once the entry returns.
struct ThreadContext final {
  NativeThread::EntryFn entry = nullptr;
  void *userData = nullptr;
};

#ifdef _WIN32
unsigned __stdcall thread_trampoline(void *rawContext) noexcept {
  auto *context = static_cast<ThreadContext *>(rawContext);
  context->entry(context->userData);
  delete context;
  return 0U;
}
#else
void *thread_trampoline(void *rawContext) noexcept {
  auto *context = static_cast<ThreadContext *>(rawContext);
  context->entry(context->userData);
  delete context;
  return nullptr;
}
#endif

} // namespace

NativeThread::~NativeThread() noexcept {
  // A still-running worker at destruction is a caller bug; joining is
  // the only termination-free recovery available here.
  join();
}

NativeThread::NativeThread(NativeThread &&other) noexcept {
  m_handle = other.m_handle;
  other.m_handle = nullptr;
#ifndef _WIN32
  m_thread = other.m_thread;
  other.m_thread = 0U;
#endif
}

NativeThread &NativeThread::operator=(NativeThread &&other) noexcept {
  if (this != &other) {
    join();
    m_handle = other.m_handle;
    other.m_handle = nullptr;
#ifndef _WIN32
    m_thread = other.m_thread;
    other.m_thread = 0U;
#endif
  }
  return *this;
}

bool NativeThread::spawn(EntryFn entry, void *userData) noexcept {
  if ((entry == nullptr) || joinable()) {
    return false;
  }

  auto *context = new (std::nothrow) ThreadContext{entry, userData};
  if (context == nullptr) {
    return false;
  }

#ifdef _WIN32
  const std::uintptr_t handle =
      _beginthreadex(nullptr, 0U, &thread_trampoline, context, 0U, nullptr);
  if (handle == 0U) {
    delete context;
    return false;
  }
  m_handle = reinterpret_cast<void *>(handle);
  return true;
#else
  static_assert(sizeof(pthread_t) <= sizeof(m_thread),
                "pthread_t must fit the stored representation");
  pthread_t thread{};
  if (pthread_create(&thread, nullptr, &thread_trampoline, context) != 0) {
    delete context;
    return false;
  }
  std::memcpy(&m_thread, &thread, sizeof(thread));
  // Non-null marker: POSIX validity rides m_handle so joinable() has one
  // definition on every platform.
  m_handle = this;
  return true;
#endif
}

bool NativeThread::joinable() const noexcept { return m_handle != nullptr; }

void NativeThread::join() noexcept {
  if (m_handle == nullptr) {
    return;
  }
#ifdef _WIN32
  WaitForSingleObject(static_cast<HANDLE>(m_handle), INFINITE);
  CloseHandle(static_cast<HANDLE>(m_handle));
#else
  pthread_t thread{};
  std::memcpy(&thread, &m_thread, sizeof(thread));
  pthread_join(thread, nullptr);
  m_thread = 0U;
#endif
  m_handle = nullptr;
}

} // namespace engine::core
