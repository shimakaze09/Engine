// Declares the one command-line parser every engine executable shares: a
// caller lists the long options it accepts, and argv is split into those
// options and the positional arguments without allocating or copying.
// Options are GNU-style long options only ("--name", "--name value",
// "--name=value"); "--" ends the options, so a positional may start with
// dashes. Anything the caller did not declare is refused, never ignored:
// a mistyped option that is silently dropped runs a tool on the wrong
// input, which Godot's and Unreal's command lines both learned to reject.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace engine::core {

/// How many options one parser can declare and how many positional
/// arguments it can hold; both are fixed so parsing never allocates.
inline constexpr std::size_t kMaxCommandLineOptions = 16U;
inline constexpr std::size_t kMaxCommandLinePositionals = 64U;

/// Whether an option stands alone or takes the next word (or the text
/// after '=') as its value.
enum class CommandLineOptionKind : std::uint8_t { Flag, Value };

/// One declared option. `name` is spelled without the leading "--" and
/// must outlive the parsed CommandLine, which looks options up by it; a
/// static table is the intended owner.
struct CommandLineOption final {
  const char *name = "";
  CommandLineOptionKind kind = CommandLineOptionKind::Flag;
};

/// Why a command line was refused.
enum class CommandLineFailureKind : std::uint8_t {
  /// An argument starting with '-' names no declared option.
  UnknownOption,
  /// A Value option is the last argument, with nothing after it.
  MissingValue,
  /// A Flag was given a value through "--flag=value".
  UnexpectedValue,
  /// The same option appears twice; which one wins would be a guess.
  DuplicateOption,
  /// More positional arguments than the caller accepts.
  TooManyPositionals,
  /// The caller's own table is unusable: more than kMaxCommandLineOptions
  /// entries, an empty or dash-led name, or a name declared twice.
  InvalidSpec,
};

/// The refusal and the argv index of the argument that caused it (0 for
/// InvalidSpec, which no argument caused).
struct CommandLineFailure final {
  CommandLineFailureKind kind = CommandLineFailureKind::UnknownOption;
  int argumentIndex = 0;
};

/// A parsed command line. Values and positionals point into argv, which
/// outlives main's callees, so nothing is copied.
class CommandLine final {
public:
  /// True when the declared option `name` was given. False for a name the
  /// parser was not given, as for one that was simply absent.
  [[nodiscard]] bool has(const char *name) const noexcept;
  /// The value of the declared Value option `name`, or null when it was
  /// not given (or `name` is a Flag or undeclared).
  [[nodiscard]] const char *value(const char *name) const noexcept;
  [[nodiscard]] std::size_t positional_count() const noexcept {
    return m_positionalCount;
  }
  /// The positional argument at `index`, or null past the end.
  [[nodiscard]] const char *positional(std::size_t index) const noexcept;

private:
  friend std::expected<CommandLine, CommandLineFailure>
  parse_command_line(int argc, const char *const *argv,
                     const CommandLineOption *options,
                     std::size_t optionCount,
                     std::size_t maxPositionals) noexcept;

  [[nodiscard]] std::size_t find(const char *name) const noexcept;

  const CommandLineOption *m_options = nullptr;
  std::size_t m_optionCount = 0U;
  std::array<bool, kMaxCommandLineOptions> m_present =
      std::array<bool, kMaxCommandLineOptions>();
  std::array<const char *, kMaxCommandLineOptions> m_values =
      std::array<const char *, kMaxCommandLineOptions>();
  std::array<const char *, kMaxCommandLinePositionals> m_positionals =
      std::array<const char *, kMaxCommandLinePositionals>();
  std::size_t m_positionalCount = 0U;
};

/// Splits argv[1..argc) into the declared `options` and at most
/// `maxPositionals` positional arguments (clamped to
/// kMaxCommandLinePositionals). argv[0], the program, is skipped. A lone
/// "-" is a positional (the conventional name for stdin). The first
/// failure is returned and nothing else is reported.
std::expected<CommandLine, CommandLineFailure>
parse_command_line(int argc, const char *const *argv,
                   const CommandLineOption *options, std::size_t optionCount,
                   std::size_t maxPositionals) noexcept;

/// A short English description of `kind`, for a tool's error line.
const char *command_line_failure_text(CommandLineFailureKind kind) noexcept;

} // namespace engine::core
