// Verifies the named log channels: every channel has a non-empty name,
// no two channels share one, the enum overload of log_message reaches a
// sink under exactly that name, and the scripting channel is spelled one
// way across the tree (a sink that saw "Scripting" or "script" would
// escape the console's Script filter).

#include <cstdio>
#include <cstring>

#include "engine/core/logging.h"

namespace {

int g_failures = 0;

void check(bool condition, const char *name) noexcept {
  if (!condition) {
    std::printf("FAIL: %s\n", name);
    ++g_failures;
  }
}

constexpr engine::core::LogChannel kChannels[] = {
    engine::core::LogChannel::Engine,
    engine::core::LogChannel::Runtime,
    engine::core::LogChannel::World,
    engine::core::LogChannel::Renderer,
    engine::core::LogChannel::RenderDevice,
    engine::core::LogChannel::RenderPrep,
    engine::core::LogChannel::Shader,
    engine::core::LogChannel::Shadow,
    engine::core::LogChannel::ShadowMap,
    engine::core::LogChannel::PassResources,
    engine::core::LogChannel::Bgfx,
    engine::core::LogChannel::Scripting,
    engine::core::LogChannel::Dap,
    engine::core::LogChannel::Editor,
    engine::core::LogChannel::Assets,
    engine::core::LogChannel::AssetStreaming,
    engine::core::LogChannel::Streaming,
    engine::core::LogChannel::Save,
    engine::core::LogChannel::Prefab,
    engine::core::LogChannel::Audio,
    engine::core::LogChannel::Physics,
    engine::core::LogChannel::Animation,
    engine::core::LogChannel::EntityPool,
    engine::core::LogChannel::Jobs,
    engine::core::LogChannel::Slice,
};
constexpr std::size_t kChannelCount = sizeof(kChannels) / sizeof(kChannels[0]);

char g_lastChannel[64] = {};
char g_lastMessage[128] = {};

void record_sink(engine::core::LogLevel, const char *channel,
                 const char *message, void *) noexcept {
  std::snprintf(g_lastChannel, sizeof(g_lastChannel), "%s", channel);
  std::snprintf(g_lastMessage, sizeof(g_lastMessage), "%s", message);
}

} // namespace

int main() {
  for (std::size_t i = 0U; i < kChannelCount; ++i) {
    const char *name = engine::core::log_channel_name(kChannels[i]);
    check((name != nullptr) && (name[0] != '\0'), "channel has a name");
    for (std::size_t j = i + 1U; j < kChannelCount; ++j) {
      check(std::strcmp(name, engine::core::log_channel_name(kChannels[j])) !=
                0,
            "channel names are distinct");
    }
    for (const char *c = name; *c != '\0'; ++c) {
      check(((*c >= 'a') && (*c <= 'z')) || (*c == '_'),
            "channel names are lower-case identifiers");
    }
  }
  check(std::strcmp(engine::core::log_channel_name(
                        engine::core::LogChannel::Scripting),
                    "scripting") == 0,
        "the scripting channel is spelled scripting");

  check(engine::core::initialize_logging(), "initialize logging");
  check(engine::core::log_register_sink(&record_sink, nullptr),
        "register sink");
  engine::core::log_message(engine::core::LogLevel::Info,
                            engine::core::LogChannel::Scripting, "hello");
  check(std::strcmp(g_lastChannel, "scripting") == 0,
        "the enum overload reaches the sink under the channel's name");
  check(std::strcmp(g_lastMessage, "hello") == 0,
        "the enum overload forwards the message unchanged");
  engine::core::log_unregister_sink(&record_sink, nullptr);
  engine::core::shutdown_logging();

  if (g_failures != 0) {
    std::printf("log channel tests: %d failure(s)\n", g_failures);
    return 1;
  }
  return 0;
}
