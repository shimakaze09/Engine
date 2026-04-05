#pragma once

#include <cstdint>

namespace engine {

bool bootstrap() noexcept;
void run(std::uint32_t maxFrames = 0U) noexcept;
void shutdown() noexcept;

} // namespace engine
