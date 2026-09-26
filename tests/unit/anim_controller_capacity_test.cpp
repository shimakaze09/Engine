// Verifies the animation controller capacities on cooked fixtures: a
// character-sized controller (128 clips, 64 states, 256 transitions) and
// one at every per-controller ceiling load with exactly their declared
// counts and play every state through parameter-driven transitions; one
// past any ceiling is refused whole with an Error naming the file and the
// limit; every animation component a World holds can bind a controller of
// its own; and the registry holds kMaxAnimControllers distinct controllers,
// refusing the next one with a diagnostic.

#include "engine/runtime/animation_system.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "anim_cook.h"
#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/renderer/command_buffer.h"
#include "engine/runtime/world.h"

namespace {

using engine::runtime::AnimationComponent;
using engine::runtime::kInvalidAnimSlot;
using engine::runtime::kMaxAnimClips;
using engine::runtime::kMaxAnimControllers;
using engine::runtime::kMaxAnimEvents;
using engine::runtime::kMaxAnimStates;
using engine::runtime::kMaxAnimTransitions;
namespace math = engine::math;
namespace tools = engine::tools;

constexpr const char *kSkelPath = "anim_capacity_test.skel";
constexpr const char *kClipPath = "anim_capacity_test.anim";
constexpr const char *kMountPrefix = "animcap";
constexpr float kFixedDt = 1.0F / 60.0F;

std::vector<std::string> g_writtenFiles{};

/// Writes a fixture file and remembers it for cleanup.
bool write_text(const std::string &path, const std::string &text) {
  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path.c_str(), "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path.c_str(), "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  g_writtenFiles.push_back(path);
  const bool ok =
      std::fwrite(text.data(), 1U, text.size(), file) == text.size();
  return (std::fclose(file) == 0) && ok;
}

void cleanup_files() {
  for (const std::string &path : g_writtenFiles) {
    static_cast<void>(std::remove(path.c_str()));
  }
  static_cast<void>(std::remove(kSkelPath));
  static_cast<void>(std::remove(kClipPath));
}

/// Cooks a two-joint skeleton and one 0.5 s clip every controller shares,
/// then mounts the working directory for the runtime loaders.
bool cook_fixtures() {
  tools::Skeleton skeleton{};
  skeleton.joints.resize(2U);
  skeleton.joints[0].name = "root";
  skeleton.joints[0].parent = tools::kInvalidSkeletonJoint;
  skeleton.joints[1].name = "tip";
  skeleton.joints[1].parent = 0U;
  skeleton.joints[1].restTranslation = math::Vec3(0.0F, 1.0F, 0.0F);
  skeleton.rootJoint = 0U;
  for (tools::SkeletonJoint &joint : skeleton.joints) {
    joint.inverseBindMatrix = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                               0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
  }
  std::vector<std::uint32_t> remap{};
  if (!tools::reorder_skeleton_parent_first(&skeleton, &remap) ||
      !tools::write_skeleton_asset(kSkelPath, skeleton)) {
    return false;
  }

  tools::AnimClip clip{};
  clip.name = "clip";
  clip.durationSeconds = 0.5F;
  tools::AnimTrack track{};
  track.joint = 1U;
  track.target = tools::AnimTrackTarget::Translation;
  track.interpolation = tools::AnimInterpolation::Linear;
  track.times = {0.0F, 0.5F};
  track.vec3Values = {math::Vec3(0.0F, 1.0F, 0.0F),
                      math::Vec3(0.0F, 1.0F, 1.0F)};
  clip.tracks.push_back(track);
  if (!tools::write_anim_clip_asset(kClipPath, clip, remap)) {
    return false;
  }
  return engine::core::initialize_vfs() &&
         engine::core::mount(kMountPrefix, ".");
}

/// Counts of each controller table a generated fixture declares.
struct ControllerShape final {
  std::size_t clips = 1U;
  std::size_t states = 1U;
  std::size_t transitions = 0U;
  std::size_t events = 0U;
};

/// Controller JSON of the given shape. Clip i and state i pair up; the
/// first states - 1 transitions step state i to i + 1 when "step" equals
/// i + 1, and the rest never fire. Events sit on clip 0 at 0.4 s.
std::string controller_json(const ControllerShape &shape) {
  std::string json = "{\"skeleton\":\"animcap/anim_capacity_test.skel\",";
  json += "\"clips\":[";
  for (std::size_t i = 0U; i < shape.clips; ++i) {
    json += (i == 0U) ? "" : ",";
    json += "{\"name\":\"c" + std::to_string(i) +
            "\",\"path\":\"animcap/anim_capacity_test.anim\"}";
  }
  json += "],\"states\":[";
  for (std::size_t i = 0U; i < shape.states; ++i) {
    json += (i == 0U) ? "" : ",";
    json += "{\"name\":\"s" + std::to_string(i) + "\",\"clip\":\"c" +
            std::to_string(i % shape.clips) + "\",\"loop\":true}";
  }
  json += "],\"transitions\":[";
  for (std::size_t i = 0U; i < shape.transitions; ++i) {
    json += (i == 0U) ? "" : ",";
    if ((i + 1U) < shape.states) {
      json += "{\"from\":\"s" + std::to_string(i) + "\",\"to\":\"s" +
              std::to_string(i + 1U) +
              "\",\"param\":\"step\",\"when\":\"==\",\"value\":" +
              std::to_string(i + 1U) + ",\"blend\":0}";
    } else {
      json += "{\"from\":\"any\",\"to\":\"s0\",\"param\":\"never\","
              "\"when\":\">\",\"value\":1,\"blend\":0}";
    }
  }
  json += "],\"events\":[";
  for (std::size_t i = 0U; i < shape.events; ++i) {
    json += (i == 0U) ? "" : ",";
    json += "{\"clip\":\"c0\",\"time\":0.4,\"name\":\"e" + std::to_string(i) +
            "\"}";
  }
  json += "]}";
  return json;
}

/// Writes a controller of `shape` as <name>.animctrl; returns its VFS path,
/// empty when the write failed.
std::string write_controller(const std::string &name,
                             const ControllerShape &shape) {
  const std::string file = name + ".animctrl";
  if (!write_text(file, controller_json(shape))) {
    return std::string();
  }
  return std::string(kMountPrefix) + "/" + file;
}

/// Error lines on the animation channel while the sink is registered.
std::vector<std::string> g_errors{};

void capture_errors(engine::core::LogLevel level, const char *channel,
                    const char *message, void *) noexcept {
  if ((level == engine::core::LogLevel::Error) &&
      (std::strcmp(channel, "animation") == 0) && (message != nullptr)) {
    g_errors.emplace_back(message);
  }
}

/// True when some captured Error names both `path` and `text`.
bool error_names(const std::string &path, const char *text) {
  for (const std::string &line : g_errors) {
    if ((line.find(path) != std::string::npos) &&
        (line.find(text) != std::string::npos)) {
      return true;
    }
  }
  return false;
}

/// EXPECTATION: a controller of `shape` loads with exactly its declared
/// counts, and a live entity steps through every one of its states,
/// drawing a palette on each.
int check_controller_plays_every_state(const char *name,
                                       const ControllerShape &shape) {
  engine::runtime::reset_anim_controllers();
  const std::string path = write_controller(name, shape);
  if (path.empty()) {
    std::printf("could not write %s\n", name);
    return 1;
  }
  const std::uint32_t slot =
      engine::runtime::acquire_anim_controller(path.c_str());
  const engine::runtime::AnimControllerData *controller =
      engine::runtime::get_anim_controller(slot);
  if (controller == nullptr) {
    std::printf("%s did not load\n", name);
    return 1;
  }
  if ((controller->clipCount != shape.clips) ||
      (controller->stateCount != shape.states) ||
      (controller->transitionCount != shape.transitions) ||
      (controller->eventCount != shape.events) ||
      (controller->clips.size() != shape.clips) ||
      (controller->states.size() != shape.states) ||
      (controller->transitions.size() != shape.transitions) ||
      (controller->events.size() != shape.events)) {
    std::printf("%s's counts differ from its asset\n", name);
    return 1;
  }

  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 1;
  }
  world->end_frame_phase();
  const auto entity = world->create_entity();
  AnimationComponent component{};
  std::snprintf(component.controllerPath, sizeof(component.controllerPath),
                "%s", path.c_str());
  if (!world->add_animation_component(entity, component)) {
    std::puts("add_animation_component failed");
    return 1;
  }
  engine::runtime::update_animations(*world, kFixedDt);
  for (std::size_t state = 0U; state < shape.states; ++state) {
    if ((state > 0U) &&
        !engine::runtime::set_anim_param(*world, entity, "step",
                                         static_cast<float>(state))) {
      std::puts("set_anim_param failed");
      return 1;
    }
    if (state > 0U) {
      engine::runtime::update_animations(*world, kFixedDt);
    }
    const AnimationComponent *live = world->get_animation_component_ptr(entity);
    if ((live == nullptr) || (live->currentState != state) ||
        (live->paletteSlot == kInvalidAnimSlot)) {
      std::printf("%s: state %zu was not reached and drawn\n", name, state);
      return 1;
    }
  }
  engine::renderer::set_skin_palettes(nullptr, 0U);
  return 0;
}

/// EXPECTATION: one past any single ceiling refuses the whole controller
/// with an Error naming the file and the limit.
int check_one_past_each_ceiling_is_refused() {
  const struct {
    const char *name;
    ControllerShape shape;
  } cases[] = {
      {"anim_capacity_clips", {kMaxAnimClips + 1U, 1U, 0U, 0U}},
      {"anim_capacity_states", {1U, kMaxAnimStates + 1U, 0U, 0U}},
      {"anim_capacity_transitions", {1U, 1U, kMaxAnimTransitions + 1U, 0U}},
      {"anim_capacity_events", {1U, 1U, 0U, kMaxAnimEvents + 1U}},
  };
  for (const auto &row : cases) {
    engine::runtime::reset_anim_controllers();
    const std::string path = write_controller(row.name, row.shape);
    if (path.empty()) {
      std::printf("could not write %s\n", row.name);
      return 1;
    }
    g_errors.clear();
    const bool sinkOk =
        engine::core::log_register_sink(&capture_errors, nullptr);
    const std::uint32_t slot =
        engine::runtime::acquire_anim_controller(path.c_str());
    if (sinkOk) {
      engine::core::log_unregister_sink(&capture_errors, nullptr);
    }
    if (slot != kInvalidAnimSlot) {
      std::printf("%s past its ceiling loaded\n", row.name);
      return 1;
    }
    if (!sinkOk || !error_names(path, "the limit is")) {
      std::printf("%s was refused without naming the file and the limit\n",
                  row.name);
      return 1;
    }
  }
  return 0;
}

/// EXPECTATION: every animation component a World holds binds a distinct
/// controller of its own in one update.
int check_every_component_binds_its_own_controller() {
  engine::runtime::reset_anim_controllers();
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 1;
  }
  world->end_frame_phase();
  constexpr std::size_t kComponents =
      engine::runtime::World::kMaxAnimationComponents;
  std::vector<engine::core::Entity> entities{};
  for (std::size_t i = 0U; i < kComponents; ++i) {
    const std::string path = write_controller(
        "anim_capacity_character_" + std::to_string(i), ControllerShape{});
    const auto entity = world->create_entity();
    AnimationComponent component{};
    std::snprintf(component.controllerPath, sizeof(component.controllerPath),
                  "%s", path.c_str());
    if (path.empty() || !world->add_animation_component(entity, component)) {
      std::puts("could not add an animated character");
      return 1;
    }
    entities.push_back(entity);
  }
  engine::runtime::update_animations(*world, kFixedDt);
  std::size_t bound = 0U;
  for (const engine::core::Entity entity : entities) {
    const AnimationComponent *live = world->get_animation_component_ptr(entity);
    if ((live != nullptr) && (live->controllerSlot != kInvalidAnimSlot)) {
      ++bound;
    }
  }
  engine::renderer::set_skin_palettes(nullptr, 0U);
  if (bound != kComponents) {
    std::printf("%zu of %zu characters bound their controller\n", bound,
                kComponents);
    return 1;
  }
  return 0;
}

/// EXPECTATION: kMaxAnimControllers distinct controllers load into distinct
/// slots, and the next distinct one is refused with a diagnostic while the
/// loaded ones stay readable.
int check_registry_capacity() {
  engine::runtime::reset_anim_controllers();
  std::vector<std::uint32_t> slots{};
  for (std::size_t i = 0U; i <= kMaxAnimControllers; ++i) {
    const std::string path = write_controller(
        "anim_capacity_registry_" + std::to_string(i), ControllerShape{});
    if (path.empty()) {
      std::puts("could not write a registry controller");
      return 1;
    }
    if (i < kMaxAnimControllers) {
      const std::uint32_t slot =
          engine::runtime::acquire_anim_controller(path.c_str());
      if (slot == kInvalidAnimSlot) {
        std::printf("controller %zu of %zu did not load\n", i + 1U,
                    kMaxAnimControllers);
        return 1;
      }
      slots.push_back(slot);
      continue;
    }
    g_errors.clear();
    const bool sinkOk =
        engine::core::log_register_sink(&capture_errors, nullptr);
    const std::uint32_t refused =
        engine::runtime::acquire_anim_controller(path.c_str());
    if (sinkOk) {
      engine::core::log_unregister_sink(&capture_errors, nullptr);
    }
    if (refused != kInvalidAnimSlot) {
      std::puts("a controller past the registry loaded");
      return 1;
    }
    if (!sinkOk || !error_names(path, "controller slots exhausted")) {
      std::puts("the registry refusal logged no diagnostic");
      return 1;
    }
  }
  for (std::size_t i = 0U; i < slots.size(); ++i) {
    for (std::size_t j = i + 1U; j < slots.size(); ++j) {
      if (slots[i] == slots[j]) {
        std::puts("two controllers share a slot");
        return 1;
      }
    }
    if (engine::runtime::get_anim_controller(slots[i]) == nullptr) {
      std::puts("a loaded controller became unreadable");
      return 1;
    }
  }
  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  static_cast<void>(engine::core::initialize_logging());
  if (!cook_fixtures()) {
    cleanup_files();
    std::puts("fixture cook failed");
    return 1;
  }
  // A third-person character: idle, locomotion, jumps, attack chains,
  // hits, death and emotes, each with its own clip.
  int result = check_controller_plays_every_state(
      "anim_capacity_character", ControllerShape{128U, 64U, 256U, 64U});
  if (result == 0) {
    result = check_controller_plays_every_state(
        "anim_capacity_full",
        ControllerShape{kMaxAnimClips, kMaxAnimStates, kMaxAnimTransitions,
                        kMaxAnimEvents});
  }
  if (result == 0) {
    result = check_every_component_binds_its_own_controller();
  }
  if (result == 0) {
    result = check_one_past_each_ceiling_is_refused();
  }
  if (result == 0) {
    result = check_registry_capacity();
  }
  engine::runtime::reset_anim_controllers();
  cleanup_files();
  return result;
}
