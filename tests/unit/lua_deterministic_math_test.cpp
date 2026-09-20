// Pins that a script's math.sin, cos, tan, exp, atan and log produce the
// promised words of scalar_pinned_words.h through the production VM: the
// script evaluates each function on the pinned inputs, packs the result
// as a float and logs its bits, and the test compares them to the table
// the C++ suite also holds. On a VM whose math table still reaches the C
// library the words differ on at least one lane (on Linux, exp(0.35),
// sin(-2.5) and four atan inputs). Also pins the two-argument atan and
// the two-argument log against the scalar set directly.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../test_harness.h"
#include "engine/core/logging.h"
#include "engine/math/scalar.h"
#include "engine/scripting/scripting.h"
#include "scalar_pinned_words.h"

namespace {

constexpr const char *kScriptPath = "lua_deterministic_math_test.lua";
constexpr int kMaxLines = 64;

/// One "DETMATH <name> <bits>" line the script logged.
struct Report final {
  char name[32] = {};
  std::uint32_t bits = 0U;
};

Report g_reports[kMaxLines]{};
int g_reportCount = 0;

void capture(engine::core::LogLevel, const char *channel, const char *message,
             void *) noexcept {
  if ((channel == nullptr) || (std::strcmp(channel, "scripting") != 0) ||
      (message == nullptr) ||
      (std::strncmp(message, "DETMATH ", 8U) != 0) ||
      (g_reportCount >= kMaxLines)) {
    return;
  }
  // Parsed by hand rather than with sscanf, which the MSVC runtime
  // deprecates and the Windows lanes reject under /WX.
  const char *cursor = message + 8;
  const char *space = std::strchr(cursor, ' ');
  Report report{};
  if ((space == nullptr) || (space == cursor) ||
      (static_cast<std::size_t>(space - cursor) >= sizeof(report.name))) {
    return;
  }
  std::memcpy(report.name, cursor, static_cast<std::size_t>(space - cursor));
  char *end = nullptr;
  const unsigned long parsed = std::strtoul(space + 1, &end, 16);
  if ((end == (space + 1)) || (*end != '\0')) {
    return;
  }
  report.bits = static_cast<std::uint32_t>(parsed);
  g_reports[g_reportCount] = report;
  ++g_reportCount;
}

bool write_script(const char *code) noexcept {
  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, kScriptPath, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(kScriptPath, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t length = std::strlen(code);
  const bool ok = (std::fwrite(code, 1U, length, file) == length);
  std::fclose(file);
  return ok;
}

/// The bits the script reported under `name`; false when it never did.
bool reported(const char *name, std::uint32_t *outBits) noexcept {
  for (int i = 0; i < g_reportCount; ++i) {
    if (std::strcmp(g_reports[i].name, name) == 0) {
      *outBits = g_reports[i].bits;
      return true;
    }
  }
  return false;
}

std::uint32_t bits_of(float value) noexcept {
  std::uint32_t out = 0U;
  std::memcpy(&out, &value, sizeof(out));
  return out;
}

// The script names each report "<function><index>" so the test can look
// it up per pinned row; the bits are the float the Lua number rounds to.
constexpr const char *kScript =
    "local inputs = {0.0, 0.005, -0.0066666668, 0.35, 1.0, -2.5, 7.0, -12.0}\n"
    "local function bits(v)\n"
    "  return string.format('%08X', string.unpack('<I4', string.pack('<f', v)))\n"
    "end\n"
    "for i, x in ipairs(inputs) do\n"
    "  engine.log('DETMATH sin' .. i .. ' ' .. bits(math.sin(x)))\n"
    "  engine.log('DETMATH cos' .. i .. ' ' .. bits(math.cos(x)))\n"
    "  engine.log('DETMATH exp' .. i .. ' ' .. bits(math.exp(x)))\n"
    "  engine.log('DETMATH atan' .. i .. ' ' .. bits(math.atan(x)))\n"
    "  engine.log('DETMATH tan' .. i .. ' ' .. bits(math.tan(x)))\n"
    "  if x > 0 then\n"
    "    engine.log('DETMATH log' .. i .. ' ' .. bits(math.log(x)))\n"
    "  end\n"
    "end\n"
    "engine.log('DETMATH atan_yx ' .. bits(math.atan(1.0, -1.0)))\n"
    "engine.log('DETMATH log_base ' .. bits(math.log(8.0, 2)))\n"
    "engine.log('DETMATH asin ' .. bits(math.asin(0.35)))\n"
    "engine.log('DETMATH acos ' .. bits(math.acos(0.35)))\n";

} // namespace

/// Runs this executable or test program.
int main() {
  engine::tests::TestContext ctx;
  ctx.check(engine::core::initialize_logging(), "initialize logging");
  ctx.check(engine::core::log_register_sink(&capture, nullptr),
            "register the log sink");
  ctx.check(engine::scripting::initialize_scripting(), "initialize scripting");
  ctx.check(write_script(kScript), "write the script");
  ctx.check(engine::scripting::load_script(kScriptPath), "the script runs");

  for (int i = 0; i < engine::tests::kPinnedScalarWordCount; ++i) {
    const engine::tests::PinnedScalarWords &row =
        engine::tests::kPinnedScalarWords[i];
    struct Expect final {
      const char *function;
      std::uint32_t bits;
      bool applies;
    };
    const Expect expects[] = {
        {"sin", row.sinBits, true},   {"cos", row.cosBits, true},
        {"exp", row.expBits, true},   {"atan", row.atanBits, true},
        {"tan", row.tanBits, true},   {"log", row.logBits, row.input > 0.0F},
    };
    for (const Expect &expect : expects) {
      if (!expect.applies) {
        continue;
      }
      char name[32] = {};
      std::snprintf(name, sizeof(name), "%s%d", expect.function, i + 1);
      char label[96] = {};
      std::snprintf(label, sizeof(label), "math.%s(%g) is the pinned word",
                    expect.function, static_cast<double>(row.input));
      std::uint32_t bits = 0U;
      const bool ok = reported(name, &bits) && (bits == expect.bits);
      if (!ok) {
        std::printf("  %s: script %08X pinned %08X\n", label, bits,
                    expect.bits);
      }
      ctx.check(ok, label);
    }
  }

  std::uint32_t bits = 0U;
  ctx.check(reported("atan_yx", &bits) &&
                (bits == bits_of(engine::math::det_atan2(1.0F, -1.0F))),
            "math.atan(y, x) is the deterministic atan2");
  ctx.check(reported("log_base", &bits) &&
                (bits == bits_of(engine::math::det_log(8.0F) /
                                 engine::math::det_log(2.0F))),
            "math.log(x, base) is the quotient of deterministic logs");
  ctx.check(reported("asin", &bits) &&
                (bits == bits_of(engine::math::det_asin(0.35F))),
            "math.asin is the deterministic asin");
  ctx.check(reported("acos", &bits) &&
                (bits == bits_of(engine::math::det_acos(0.35F))),
            "math.acos is the deterministic acos");

  engine::scripting::shutdown_scripting();
  engine::core::log_unregister_sink(&capture, nullptr);
  engine::core::shutdown_logging();
  static_cast<void>(std::remove(kScriptPath));
  return ctx.finish("lua deterministic math");
}
