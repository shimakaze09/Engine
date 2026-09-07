// Benchmarks the two cvar read paths against a registry filled to the
// size a running engine carries: the by-name path (mutex plus a string
// scan of the table) and the handle path (lock-free atomic loads). The
// gate metric is their ratio, handle cost over name cost, which is
// machine-independent enough to hold on any CI runner and pins the hot-path
// property the renderer flush, streaming pump, and frame pipeline rely on:
// a handle read must stay a small fraction of a name read. The absolute
// nanoseconds are reported as diagnostics.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "engine/core/cvar.h"

namespace {

using Clock = std::chrono::high_resolution_clock;

// A registry as full as a running engine's (renderer, physics, audio,
// streaming, editor knobs), so the name scan pays its real cost; the
// benchmarked cvars sit at the end where a scan is longest.
constexpr std::size_t kFillerCount = 200U;
constexpr std::size_t kReadsPerSample = 20000U;
constexpr std::size_t kWarmupPasses = 3U;
constexpr std::size_t kSamplePasses = 15U;

char g_fillerNames[kFillerCount][24] = {};

/// Parses text into the engine representation for json out.
bool parse_json_out(int argc, char **argv, const char **outPath) noexcept {
  if (outPath == nullptr) {
    return false;
  }
  *outPath = nullptr;
  for (int i = 1; i < argc; ++i) {
    if ((std::strcmp(argv[i], "--json-out") == 0) && ((i + 1) < argc)) {
      *outPath = argv[i + 1];
      return true;
    }
  }
  return true;
}

/// Returns the median of count samples, sorting them in place.
double median_of(double *samples, std::size_t count) noexcept {
  std::sort(samples, samples + count);
  const std::size_t mid = count / 2U;
  if ((count % 2U) == 0U) {
    return 0.5 * (samples[mid - 1U] + samples[mid]);
  }
  return samples[mid];
}

/// Fills the registry and registers the three benchmarked cvars last.
bool populate_registry() noexcept {
  if (!engine::core::initialize_cvars()) {
    return false;
  }
  for (std::size_t i = 0U; i < kFillerCount; ++i) {
    std::snprintf(g_fillerNames[i], sizeof(g_fillerNames[i]),
                  "bench.filler_%03zu", i);
    if (!engine::core::cvar_register_int(g_fillerNames[i],
                                         static_cast<int>(i), "filler")) {
      return false;
    }
  }
  return engine::core::cvar_register_bool("bench.flag", true, "bench") &&
         engine::core::cvar_register_int("bench.count", 3, "bench") &&
         engine::core::cvar_register_float("bench.scale", 0.5F, "bench");
}

/// One sample of the by-name path: the three reads the flush shape does
/// (bool, int, float), kReadsPerSample times. The checksum keeps the reads
/// observable so the compiler cannot drop them.
double measure_name_reads_ns(double *checksum) noexcept {
  const auto start = Clock::now();
  double sum = 0.0;
  for (std::size_t i = 0U; i < kReadsPerSample; ++i) {
    sum += engine::core::cvar_get_bool("bench.flag", false) ? 1.0 : 0.0;
    sum += engine::core::cvar_get_int("bench.count", 0);
    sum += engine::core::cvar_get_float("bench.scale", 0.0F);
  }
  const auto end = Clock::now();
  *checksum += sum;
  return std::chrono::duration<double, std::nano>(end - start).count() /
         static_cast<double>(kReadsPerSample * 3U);
}

/// One sample of the handle path over the same three reads.
double measure_handle_reads_ns(double *checksum) noexcept {
  engine::core::CVarRef flag{"bench.flag"};
  engine::core::CVarRef count{"bench.count"};
  engine::core::CVarRef scale{"bench.scale"};
  const auto start = Clock::now();
  double sum = 0.0;
  for (std::size_t i = 0U; i < kReadsPerSample; ++i) {
    sum += flag.get_bool(false) ? 1.0 : 0.0;
    sum += count.get_int(0);
    sum += scale.get_float(0.0F);
  }
  const auto end = Clock::now();
  *checksum += sum;
  return std::chrono::duration<double, std::nano>(end - start).count() /
         static_cast<double>(kReadsPerSample * 3U);
}

/// Warms up, then reports the median per-read cost of both paths.
bool run_benchmark(double *outNameNs, double *outHandleNs) noexcept {
  if (!populate_registry()) {
    return false;
  }

  double nameSamples[kSamplePasses] = {};
  double handleSamples[kSamplePasses] = {};
  double checksum = 0.0;
  for (std::size_t pass = 0U; pass < (kWarmupPasses + kSamplePasses); ++pass) {
    const double nameNs = measure_name_reads_ns(&checksum);
    const double handleNs = measure_handle_reads_ns(&checksum);
    if (pass >= kWarmupPasses) {
      nameSamples[pass - kWarmupPasses] = nameNs;
      handleSamples[pass - kWarmupPasses] = handleNs;
    }
  }
  engine::core::shutdown_cvars();

  // Each read contributes 1 + 3 + 0.5 per iteration on both paths.
  const double expected = 4.5 * static_cast<double>(kReadsPerSample) *
                          static_cast<double>(2U * (kWarmupPasses + kSamplePasses));
  if (checksum != expected) {
    std::printf("FAIL: cvar reads returned unexpected values\n");
    return false;
  }

  *outNameNs = median_of(nameSamples, kSamplePasses);
  *outHandleNs = median_of(handleSamples, kSamplePasses);
  return (*outNameNs > 0.0) && (*outHandleNs > 0.0);
}

/// Writes json data.
bool write_json(const char *path, double nameNs, double handleNs,
                double ratio) noexcept {
  if (path == nullptr) {
    return true;
  }

  FILE *file = nullptr;
#if defined(_MSC_VER)
  if (fopen_s(&file, path, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "wb");
#endif
  if (file == nullptr) {
    return false;
  }

  const int wrote = std::fprintf(file,
                                 "{\n"
                                 "  \"benchmark\": \"cvar_lookup\",\n"
                                 "  \"registered_cvars\": %zu,\n"
                                 "  \"reads_per_sample\": %zu,\n"
                                 "  \"cvar_name_read_ns\": %.6f,\n"
                                 "  \"cvar_handle_read_ns\": %.6f,\n"
                                 "  \"cvar_handle_read_ratio\": %.6f\n"
                                 "}\n",
                                 kFillerCount + 3U, kReadsPerSample * 3U,
                                 nameNs, handleNs, ratio);

  std::fclose(file);
  return wrote > 0;
}

} // namespace

/// Runs this executable or test program.
int main(int argc, char **argv) {
  const char *jsonOutPath = nullptr;
  if (!parse_json_out(argc, argv, &jsonOutPath)) {
    std::printf("FAIL: invalid arguments\n");
    return 1;
  }

  double nameNs = 0.0;
  double handleNs = 0.0;
  if (!run_benchmark(&nameNs, &handleNs)) {
    std::printf("FAIL: cvar lookup benchmark execution\n");
    return 1;
  }
  const double ratio = handleNs / nameNs;

  if (!write_json(jsonOutPath, nameNs, handleNs, ratio)) {
    std::printf("FAIL: writing json output\n");
    return 1;
  }

  std::printf("[cvar_lookup] cvars=%zu name_read_ns=%.2f handle_read_ns=%.2f "
              "handle/name=%.4f\n",
              kFillerCount + 3U, nameNs, handleNs, ratio);
  return 0;
}
