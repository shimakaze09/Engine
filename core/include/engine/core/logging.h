// Declares logging types and APIs for the Engine core engine.

#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::core {

/// Enumerates log level values used by the engine.
enum class LogLevel : std::uint8_t {
    Trace,
    Info,
    Warning,
    Error,
    Fatal
};

/// Returns a short human-readable name for a log level ("Trace".."Fatal").
const char *log_level_to_string(LogLevel level) noexcept;

/// Initializes the owning system for logging.
bool initialize_logging() noexcept;
/// Shuts down the owning system for logging, dropping every still-registered
/// sink under the same lifetime barrier log_unregister_sink provides.
void shutdown_logging() noexcept;
/// Writes one log line (level + channel + message) to the sinks.
void log_message(LogLevel level, const char* channel, const char* message) noexcept;
/// Logs the per-frame timing/metrics line.
void log_frame_metrics(
    std::uint32_t frameIndex,
    double frameMs,
    std::size_t frameBytes,
    std::size_t frameAllocations) noexcept;

/// Publishes the simulation frame index active while log_message runs, so
/// sinks can attach best-effort frame context; callers update it once per
/// frame (e.g. EnginePipeline). Never gates or blocks log_message itself.
void log_set_frame_index(std::uint32_t frameIndex) noexcept;
/// Returns the frame index last published by log_set_frame_index (0 before
/// the first frame or when no caller publishes one, e.g. headless tools).
std::uint32_t log_current_frame_index() noexcept;

// ---------------------------------------------------------------------------
// Generic sink registration (issue #155): lets an editor-safe consumer
// observe every log_message call without a second logging backend. Sinks
// are invoked synchronously and in registration order from whatever thread
// called log_message, including for LogLevel::Fatal (dispatched before the
// process aborts), so implementations must be reentrant-safe, must not call
// log_message themselves, and must do only fixed-size, non-blocking work
// (no heap allocation, no filesystem I/O) — mirrors the no-allocation
// contract already required of the log-ingest hot path.
// NOLINTNEXTLINE(modernize-use-using)
typedef void (*LogSinkFn)(LogLevel level, const char *channel,
                          const char *message, void *userData) noexcept;

/// Registers a sink invoked on every subsequent log_message call. Returns
/// false when the (fn, userData) pair is already registered or the fixed
/// sink table (kMaxLogSinks) is full; never allocates. A slot counts as full
/// until the sink that left it is done being dispatched, so a registration
/// racing another sink's removal can be refused while that removal drains.
bool log_register_sink(LogSinkFn fn, void *userData) noexcept;
/// Unregisters a sink previously accepted by log_register_sink; a no-op if
/// the (fn, userData) pair is not currently registered. Returns only once no
/// dispatch is still inside that sink, so the owner may release userData
/// immediately afterwards; the wait lasts at most one callback, given the
/// fixed-size non-blocking work sinks owe above. Because it waits, the caller
/// must not hold a lock its own sink acquires. A sink unregistering itself
/// from inside its own callback returns without waiting on that call, and
/// must keep its userData valid until the callback returns.
void log_unregister_sink(LogSinkFn fn, void *userData) noexcept;

/// Hard cap on simultaneously registered sinks (fixed table, no growth).
constexpr std::size_t kMaxLogSinks = 4U;

} // namespace engine::core
