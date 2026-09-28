// Verifies core::parse_command_line: flags, values given as the next word
// and after '=', positionals, "--" ending the options, a lone "-" as a
// positional, and each refusal naming the argument that caused it (an
// unknown or short option, a missing or unexpected value, a repeated
// option, one positional too many, and an unusable option table). Also
// pins the capacity boundaries: no arguments, exactly the positional
// limit, and a table of kMaxCommandLineOptions entries.

#include "engine/core/command_line.h"

#include <array>
#include <cstring>

#include "../test_harness.h"

namespace {

namespace core = engine::core;

constexpr core::CommandLineOption kOptions[] = {
    {"project", core::CommandLineOptionKind::Value},
    {"headless", core::CommandLineOptionKind::Flag},
    {"max-frames", core::CommandLineOptionKind::Value},
};
constexpr std::size_t kOptionCount = sizeof(kOptions) / sizeof(kOptions[0]);

/// Parses `args` (argv[0] included) against kOptions.
template <std::size_t N>
std::expected<core::CommandLine, core::CommandLineFailure>
parse(const std::array<const char *, N> &args, std::size_t maxPositionals) {
  return core::parse_command_line(static_cast<int>(N), args.data(), kOptions,
                                  kOptionCount, maxPositionals);
}

/// True when `result` is the failure `kind` at argv index `index`.
bool refused(const std::expected<core::CommandLine, core::CommandLineFailure>
                 &result,
             core::CommandLineFailureKind kind, int index) {
  return !result.has_value() && (result.error().kind == kind) &&
         (result.error().argumentIndex == index);
}

void test_accepts(engine::tests::TestContext &t) {
  const std::array<const char *, 1> none{"tool"};
  const auto empty = parse(none, 4U);
  t.check(empty.has_value() && (empty->positional_count() == 0U) &&
              !empty->has("project") && (empty->value("project") == nullptr),
          "no arguments parse to nothing");

  const std::array<const char *, 6> spaced{
      "tool", "--project", "game/island", "--headless", "scene.json", "-"};
  const auto a = parse(spaced, 4U);
  t.check(a.has_value() && a->has("headless") && a->has("project") &&
              (std::strcmp(a->value("project"), "game/island") == 0) &&
              (a->positional_count() == 2U) &&
              (std::strcmp(a->positional(0U), "scene.json") == 0) &&
              (std::strcmp(a->positional(1U), "-") == 0) &&
              (a->positional(2U) == nullptr),
          "a value as the next word, a flag, positionals and a lone '-'");
  t.check(a.has_value() && (a->value("headless") == nullptr) &&
              !a->has("max-frames") && !a->has("undeclared") &&
              (a->value("undeclared") == nullptr) && !a->has(nullptr),
          "a flag has no value; absent and undeclared names read as absent");

  const std::array<const char *, 3> equals{"tool", "--max-frames=120",
                                           "--project="};
  const auto b = parse(equals, 0U);
  t.check(b.has_value() &&
              (std::strcmp(b->value("max-frames"), "120") == 0) &&
              b->has("project") &&
              (std::strcmp(b->value("project"), "") == 0),
          "a value after '=', and an empty one, are taken as given");

  const std::array<const char *, 5> ended{"tool", "--headless", "--",
                                          "--project", "-x"};
  const auto c = parse(ended, 4U);
  t.check(c.has_value() && c->has("headless") && !c->has("project") &&
              (c->positional_count() == 2U) &&
              (std::strcmp(c->positional(0U), "--project") == 0) &&
              (std::strcmp(c->positional(1U), "-x") == 0),
          "'--' ends the options: what follows is positional");

  const std::array<const char *, 3> dashedValue{"tool", "--project",
                                                "--headless"};
  const auto d = parse(dashedValue, 0U);
  t.check(d.has_value() &&
              (std::strcmp(d->value("project"), "--headless") == 0) &&
              !d->has("headless"),
          "a Value option takes the next word even when it starts with '--'");
}

void test_refusals(engine::tests::TestContext &t) {
  const std::array<const char *, 3> unknown{"tool", "a", "--projct"};
  t.check(refused(parse(unknown, 4U),
                  core::CommandLineFailureKind::UnknownOption, 2),
          "a misspelled option is refused, naming its index");
  const std::array<const char *, 2> prefix{"tool", "--proj"};
  t.check(refused(parse(prefix, 4U),
                  core::CommandLineFailureKind::UnknownOption, 1),
          "a prefix of an option is not that option");
  const std::array<const char *, 2> longer{"tool", "--headlessly"};
  t.check(refused(parse(longer, 4U),
                  core::CommandLineFailureKind::UnknownOption, 1),
          "an option with extra letters is not that option");
  const std::array<const char *, 2> shortOption{"tool", "-p"};
  t.check(refused(parse(shortOption, 4U),
                  core::CommandLineFailureKind::UnknownOption, 1),
          "short options do not exist");
  const std::array<const char *, 2> missing{"tool", "--project"};
  t.check(refused(parse(missing, 4U),
                  core::CommandLineFailureKind::MissingValue, 1),
          "a Value option at the end has no value");
  const std::array<const char *, 2> flagValue{"tool", "--headless=yes"};
  t.check(refused(parse(flagValue, 4U),
                  core::CommandLineFailureKind::UnexpectedValue, 1),
          "a flag given a value is refused");
  const std::array<const char *, 5> twice{"tool", "--project", "a",
                                          "--project=b", "x"};
  t.check(refused(parse(twice, 4U),
                  core::CommandLineFailureKind::DuplicateOption, 3),
          "an option given twice is refused at the second");

  const std::array<const char *, 3> atLimit{"tool", "a", "b"};
  const auto limit = parse(atLimit, 2U);
  t.check(limit.has_value() && (limit->positional_count() == 2U),
          "exactly the positional limit is accepted");
  const std::array<const char *, 4> overLimit{"tool", "a", "b", "c"};
  t.check(refused(parse(overLimit, 2U),
                  core::CommandLineFailureKind::TooManyPositionals, 3),
          "one positional past the limit is refused at that argument");
  const std::array<const char *, 2> noneAllowed{"tool", "a"};
  t.check(refused(parse(noneAllowed, 0U),
                  core::CommandLineFailureKind::TooManyPositionals, 1),
          "a tool taking no positionals refuses one");
}

void test_option_tables(engine::tests::TestContext &t) {
  const std::array<const char *, 2> args{"tool", "--o15"};
  std::array<core::CommandLineOption, core::kMaxCommandLineOptions + 1U>
      table{};
  static const std::array<const char *, core::kMaxCommandLineOptions + 1U>
      kNames{"o0", "o1", "o2",  "o3",  "o4",  "o5",  "o6",  "o7", "o8",
             "o9", "o10", "o11", "o12", "o13", "o14", "o15", "o16"};
  for (std::size_t i = 0U; i < table.size(); ++i) {
    table[i].name = kNames[i];
  }
  const auto full = core::parse_command_line(2, args.data(), table.data(),
                                             core::kMaxCommandLineOptions, 0U);
  t.check(full.has_value() && full->has("o15"),
          "a table of exactly kMaxCommandLineOptions options works");
  const auto over = core::parse_command_line(
      2, args.data(), table.data(), core::kMaxCommandLineOptions + 1U, 0U);
  t.check(!over.has_value() &&
              (over.error().kind == core::CommandLineFailureKind::InvalidSpec),
          "one option past the table limit is an invalid table");

  const core::CommandLineOption repeated[] = {
      {"a", core::CommandLineOptionKind::Flag},
      {"a", core::CommandLineOptionKind::Value}};
  const core::CommandLineOption dashed[] = {
      {"--a", core::CommandLineOptionKind::Flag}};
  const core::CommandLineOption withEquals[] = {
      {"a=b", core::CommandLineOptionKind::Flag}};
  const core::CommandLineOption emptyName[] = {
      {"", core::CommandLineOptionKind::Flag}};
  const std::array<const char *, 1> bare{"tool"};
  t.check(!core::parse_command_line(1, bare.data(), repeated, 2U, 0U)
                   .has_value() &&
              !core::parse_command_line(1, bare.data(), dashed, 1U, 0U)
                   .has_value() &&
              !core::parse_command_line(1, bare.data(), withEquals, 1U, 0U)
                   .has_value() &&
              !core::parse_command_line(1, bare.data(), emptyName, 1U, 0U)
                   .has_value() &&
              !core::parse_command_line(1, bare.data(), nullptr, 1U, 0U)
                   .has_value(),
          "repeated, dash-led, '='-bearing, empty and missing tables are "
          "refused");
  t.check(core::parse_command_line(1, bare.data(), nullptr, 0U, 0U)
              .has_value(),
          "a tool with no options at all parses");
  t.check(std::strcmp(core::command_line_failure_text(
                          core::CommandLineFailureKind::MissingValue),
                      "option needs a value") == 0,
          "each failure has a description");
}

} // namespace

int main() {
  engine::tests::TestContext t;
  test_accepts(t);
  test_refusals(t);
  test_option_tables(t);
  return t.finish("command_line");
}
