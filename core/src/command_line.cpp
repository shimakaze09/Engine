// Implements the shared long-option command-line parser declared in
// engine/core/command_line.h.

#include "engine/core/command_line.h"

#include <cstring>

namespace engine::core {

namespace {

/// True when `argument` spells "--<name>" exactly or "--<name>=...";
/// *outValue then points after the '=' (or is null without one).
bool matches_option(const char *argument, const char *name,
                    const char **outValue) noexcept {
  const std::size_t nameLength = std::strlen(name);
  const char *spelled = argument + 2;
  if (std::strncmp(spelled, name, nameLength) != 0) {
    return false;
  }
  const char next = spelled[nameLength];
  if (next == '\0') {
    *outValue = nullptr;
    return true;
  }
  if (next == '=') {
    *outValue = spelled + nameLength + 1U;
    return true;
  }
  return false;
}

/// True when the caller's option table is one this parser can honour.
bool valid_spec(const CommandLineOption *options,
                std::size_t optionCount) noexcept {
  if ((optionCount > kMaxCommandLineOptions) ||
      ((options == nullptr) && (optionCount > 0U))) {
    return false;
  }
  for (std::size_t i = 0U; i < optionCount; ++i) {
    const char *name = options[i].name;
    if ((name == nullptr) || (name[0] == '\0') || (name[0] == '-') ||
        (std::strchr(name, '=') != nullptr)) {
      return false;
    }
    for (std::size_t j = 0U; j < i; ++j) {
      if (std::strcmp(options[j].name, name) == 0) {
        return false;
      }
    }
  }
  return true;
}

} // namespace

std::size_t CommandLine::find(const char *name) const noexcept {
  if (name == nullptr) {
    return m_optionCount;
  }
  for (std::size_t i = 0U; i < m_optionCount; ++i) {
    if (std::strcmp(m_options[i].name, name) == 0) {
      return i;
    }
  }
  return m_optionCount;
}

bool CommandLine::has(const char *name) const noexcept {
  const std::size_t index = find(name);
  return (index < m_optionCount) && m_present[index];
}

const char *CommandLine::value(const char *name) const noexcept {
  const std::size_t index = find(name);
  if ((index >= m_optionCount) ||
      (m_options[index].kind != CommandLineOptionKind::Value)) {
    return nullptr;
  }
  return m_values[index];
}

const char *CommandLine::positional(std::size_t index) const noexcept {
  return (index < m_positionalCount) ? m_positionals[index] : nullptr;
}

std::expected<CommandLine, CommandLineFailure>
parse_command_line(int argc, const char *const *argv,
                   const CommandLineOption *options, std::size_t optionCount,
                   std::size_t maxPositionals) noexcept {
  if (!valid_spec(options, optionCount)) {
    return std::unexpected(
        CommandLineFailure{CommandLineFailureKind::InvalidSpec, 0});
  }
  CommandLine parsed{};
  parsed.m_options = options;
  parsed.m_optionCount = optionCount;
  const std::size_t positionalLimit =
      (maxPositionals < kMaxCommandLinePositionals)
          ? maxPositionals
          : kMaxCommandLinePositionals;

  bool optionsEnded = false;
  for (int i = 1; (argv != nullptr) && (i < argc); ++i) {
    const char *argument = argv[i];
    if (argument == nullptr) {
      continue;
    }
    const bool looksLikeOption =
        !optionsEnded && (argument[0] == '-') && (argument[1] != '\0');
    if (!looksLikeOption) {
      if (parsed.m_positionalCount >= positionalLimit) {
        return std::unexpected(
            CommandLineFailure{CommandLineFailureKind::TooManyPositionals, i});
      }
      parsed.m_positionals[parsed.m_positionalCount] = argument;
      ++parsed.m_positionalCount;
      continue;
    }
    if (std::strcmp(argument, "--") == 0) {
      optionsEnded = true;
      continue;
    }
    // Only long options exist: "-x" is refused rather than guessed at.
    std::size_t matched = optionCount;
    const char *inlineValue = nullptr;
    if (argument[1] == '-') {
      for (std::size_t o = 0U; o < optionCount; ++o) {
        if (matches_option(argument, options[o].name, &inlineValue)) {
          matched = o;
          break;
        }
      }
    }
    if (matched == optionCount) {
      return std::unexpected(
          CommandLineFailure{CommandLineFailureKind::UnknownOption, i});
    }
    if (parsed.m_present[matched]) {
      return std::unexpected(
          CommandLineFailure{CommandLineFailureKind::DuplicateOption, i});
    }
    if (options[matched].kind == CommandLineOptionKind::Flag) {
      if (inlineValue != nullptr) {
        return std::unexpected(
            CommandLineFailure{CommandLineFailureKind::UnexpectedValue, i});
      }
    } else if (inlineValue == nullptr) {
      if (((i + 1) >= argc) || (argv[i + 1] == nullptr)) {
        return std::unexpected(
            CommandLineFailure{CommandLineFailureKind::MissingValue, i});
      }
      ++i;
      inlineValue = argv[i];
    }
    parsed.m_present[matched] = true;
    parsed.m_values[matched] = inlineValue;
  }
  return parsed;
}

const char *command_line_failure_text(CommandLineFailureKind kind) noexcept {
  switch (kind) {
  case CommandLineFailureKind::UnknownOption:
    return "unknown option";
  case CommandLineFailureKind::MissingValue:
    return "option needs a value";
  case CommandLineFailureKind::UnexpectedValue:
    return "option takes no value";
  case CommandLineFailureKind::DuplicateOption:
    return "option given twice";
  case CommandLineFailureKind::TooManyPositionals:
    return "too many arguments";
  case CommandLineFailureKind::InvalidSpec:
    return "the program's option table is invalid";
  }
  return "invalid command line";
}

} // namespace engine::core
