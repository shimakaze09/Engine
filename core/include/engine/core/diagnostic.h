// The structured diagnostic record: what a log line knows about itself
// beyond its text. Producers fill the path, line, asset or entity a
// message is about; consumers (the editor console, CI, tools) read those
// fields instead of parsing the text back out. Every field is fixed size
// so a record can be built and delivered on a noexcept path.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/logging.h"
#include "engine/core/status.h"

namespace engine::core {

/// One diagnostic. `message` is what the text line shows; the other
/// fields are its context. A zero persistent id, an empty path and a
/// negative line mean "not about one".
struct Diagnostic final {
  static constexpr std::size_t kMaxChannel = 32U;
  static constexpr std::size_t kMaxPath = 192U;
  static constexpr std::size_t kMaxField = 32U;
  static constexpr std::size_t kMaxMessage = 1024U;

  LogLevel level = LogLevel::Info;
  FailureKind kind = FailureKind::Ok;
  char channel[kMaxChannel] = {};
  /// Stamped by log_diagnostic from the published frame index.
  std::uint32_t frame = 0U;
  /// Stamped by log_diagnostic: 0 for the main thread, else the worker.
  std::uint32_t thread = 0U;
  /// Virtual path of the asset or script the message is about.
  char path[kMaxPath] = {};
  /// 1-based source line within `path`; -1 when none.
  std::int32_t line = -1;
  std::uint64_t assetId = 0U;
  std::uint32_t entityPersistentId = 0U;
  /// The serialized field or parameter the message is about.
  char field[kMaxField] = {};
  char message[kMaxMessage] = {};
};

/// Starts a record with its level, channel and message; a message longer
/// than kMaxMessage is cut, with the cut marked.
Diagnostic make_diagnostic(LogLevel level, const char *channel,
                           const char *message) noexcept;
Diagnostic make_diagnostic(LogLevel level, LogChannel channel,
                           const char *message) noexcept;

/// Copies a path into the record, cut to fit (a cut path still points at
/// the right file for a human; it is display context, not identity).
void diagnostic_set_path(Diagnostic *diagnostic, const char *path) noexcept;

/// Emits one record: prints the same text line log_message prints, hands
/// the text to the text sinks and the record, stamped with the frame and
/// thread, to the record sinks. Fatal still aborts after delivery.
void log_diagnostic(const Diagnostic &diagnostic) noexcept;

/// Emits the "<path>: <reason>" line every asset loader logs, with the
/// path also carried in the record.
void log_path_diagnostic(LogLevel level, const char *channel,
                         const char *path, const char *reason) noexcept;
void log_path_diagnostic(LogLevel level, LogChannel channel, const char *path,
                         const char *reason) noexcept;

/// A sink that receives every record log_message or log_diagnostic
/// emits, under the same contract as LogSinkFn (fixed-size, non-blocking
/// work; never log from inside). Shares the sink table and its lifetime
/// barrier with the text sinks.
// NOLINTNEXTLINE(modernize-use-using)
typedef void (*DiagnosticSinkFn)(const Diagnostic &diagnostic,
                                 void *userData) noexcept;

/// Registers a record sink; false when the pair is already registered or
/// the table is full.
bool log_register_diagnostic_sink(DiagnosticSinkFn fn, void *userData) noexcept;
/// Unregisters a record sink with log_unregister_sink's quiescence
/// guarantee.
void log_unregister_diagnostic_sink(DiagnosticSinkFn fn,
                                    void *userData) noexcept;

} // namespace engine::core
