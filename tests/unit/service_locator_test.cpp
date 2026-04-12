#include <cstdio>

#include "engine/core/service_locator.h"

static int g_testsPassed = 0;
static int g_testsFailed = 0;

static void check(bool condition, const char *name) noexcept {
  if (condition) {
    ++g_testsPassed;
  } else {
    ++g_testsFailed;
    std::fprintf(stderr, "FAIL: %s\n", name);
  }
}

struct AudioSystem final {
  int sampleRate = 44100;
};

struct PhysicsWorld final {
  float gravity = -9.8F;
};

struct Renderer final {
  int width = 1920;
  int height = 1080;
};

static bool test_register_and_retrieve() noexcept {
  engine::core::ServiceLocator loc;
  AudioSystem audio;
  audio.sampleRate = 48000;

  check(loc.register_service<AudioSystem>(&audio), "register returns true");
  check(loc.count() == 1U, "count is 1 after register");
  check(loc.has_service<AudioSystem>(), "has_service returns true");

  AudioSystem *retrieved = loc.get_service<AudioSystem>();
  check(retrieved != nullptr, "get_service returns non-null");
  check(retrieved == &audio, "get_service returns same pointer");
  check(retrieved->sampleRate == 48000, "retrieved data is correct");
  return true;
}

static bool test_get_unregistered_returns_null() noexcept {
  engine::core::ServiceLocator loc;
  check(loc.get_service<AudioSystem>() == nullptr,
        "unregistered service returns nullptr");
  check(!loc.has_service<AudioSystem>(), "has_service false for unregistered");
  return true;
}

static bool test_overwrite() noexcept {
  engine::core::ServiceLocator loc;
  AudioSystem audio1;
  audio1.sampleRate = 44100;
  AudioSystem audio2;
  audio2.sampleRate = 96000;

  loc.register_service<AudioSystem>(&audio1);
  check(loc.get_service<AudioSystem>()->sampleRate == 44100,
        "first registration");

  loc.register_service<AudioSystem>(&audio2);
  check(loc.count() == 1U, "overwrite does not increase count");
  check(loc.get_service<AudioSystem>()->sampleRate == 96000,
        "overwritten service returns new pointer");
  return true;
}

static bool test_multiple_types() noexcept {
  engine::core::ServiceLocator loc;
  AudioSystem audio;
  PhysicsWorld physics;
  Renderer renderer;

  loc.register_service<AudioSystem>(&audio);
  loc.register_service<PhysicsWorld>(&physics);
  loc.register_service<Renderer>(&renderer);

  check(loc.count() == 3U, "three services registered");
  check(loc.get_service<AudioSystem>() == &audio, "audio correct");
  check(loc.get_service<PhysicsWorld>() == &physics, "physics correct");
  check(loc.get_service<Renderer>() == &renderer, "renderer correct");
  return true;
}

static bool test_remove() noexcept {
  engine::core::ServiceLocator loc;
  AudioSystem audio;
  PhysicsWorld physics;

  loc.register_service<AudioSystem>(&audio);
  loc.register_service<PhysicsWorld>(&physics);
  check(loc.count() == 2U, "two registered");

  check(loc.remove_service<AudioSystem>(), "remove returns true");
  check(loc.count() == 1U, "count after remove");
  check(!loc.has_service<AudioSystem>(), "removed service gone");
  check(loc.has_service<PhysicsWorld>(), "other service still present");

  check(!loc.remove_service<AudioSystem>(), "double remove returns false");
  return true;
}

static bool test_clear() noexcept {
  engine::core::ServiceLocator loc;
  AudioSystem audio;
  PhysicsWorld physics;

  loc.register_service<AudioSystem>(&audio);
  loc.register_service<PhysicsWorld>(&physics);
  loc.clear();

  check(loc.count() == 0U, "count is 0 after clear");
  check(!loc.has_service<AudioSystem>(), "audio gone after clear");
  check(!loc.has_service<PhysicsWorld>(), "physics gone after clear");
  return true;
}

static bool test_type_safety() noexcept {
  // Verify that different types produce different TypeIds.
  engine::core::TypeId idAudio = engine::core::type_id<AudioSystem>();
  engine::core::TypeId idPhysics = engine::core::type_id<PhysicsWorld>();
  engine::core::TypeId idRenderer = engine::core::type_id<Renderer>();

  check(idAudio != idPhysics, "audio != physics TypeId");
  check(idAudio != idRenderer, "audio != renderer TypeId");
  check(idPhysics != idRenderer, "physics != renderer TypeId");

  // Same type produces same id.
  check(idAudio == engine::core::type_id<AudioSystem>(), "stable audio TypeId");
  return true;
}

static bool test_register_null_service() noexcept {
  engine::core::ServiceLocator loc;
  // Registering a null pointer is legal (clears the pointer for that type).
  check(loc.register_service<AudioSystem>(nullptr), "register null succeeds");
  check(loc.count() == 1U, "count is 1");
  check(loc.get_service<AudioSystem>() == nullptr,
        "get returns nullptr for null-registered");
  return true;
}

int main() {
  test_register_and_retrieve();
  test_get_unregistered_returns_null();
  test_overwrite();
  test_multiple_types();
  test_remove();
  test_clear();
  test_type_safety();
  test_register_null_service();

  std::fprintf(stdout, "ServiceLocator tests: %d passed, %d failed\n",
               g_testsPassed, g_testsFailed);
  return (g_testsFailed == 0) ? 0 : 1;
}
