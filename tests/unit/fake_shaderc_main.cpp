// Stands in for bgfx shaderc so the packer's shader-cook invocation can be
// observed without a real shader compiler: it records the argument vector it
// was launched with and writes stub bytes to the output path the cook asked
// for, so a cook driven against it reaches its commit stage. The recording is
// what the argv suite asserts on - one line per argument, so an argument that
// was split or rewritten on its way to the child is visible as a line count
// or a line body that does not match what the manifest authored.

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

/// Appends this run's argument vector to the recording file named by the
/// ENGINE_FAKE_SHADERC_ARGV environment variable; a run with the variable
/// unset records nothing.
void record_arguments(int argc, char **argv) {
  const char *logPath = std::getenv("ENGINE_FAKE_SHADERC_ARGV");
  if (logPath == nullptr) {
    return;
  }
  FILE *log = std::fopen(logPath, "ab");
  if (log == nullptr) {
    return;
  }
  // Runs append, so a cook that invoked the tool more than once is
  // distinguishable from one that invoked it once with more arguments.
  std::fprintf(log, "<run>\n");
  for (int i = 1; i < argc; ++i) {
    std::fprintf(log, "%s\n", argv[i]);
  }
  std::fclose(log);
}

/// Writes stub bytes to the path following -o; false when no output path was
/// given or the write failed.
bool write_stub_output(int argc, char **argv) {
  const char *outPath = nullptr;
  for (int i = 1; (i + 1) < argc; ++i) {
    if (std::strcmp(argv[i], "-o") == 0) {
      outPath = argv[i + 1];
    }
  }
  if (outPath == nullptr) {
    return false;
  }
  FILE *out = std::fopen(outPath, "wb");
  if (out == nullptr) {
    return false;
  }
  static const char kStub[] = "FAKE-SHADER-BINARY";
  const std::size_t written =
      std::fwrite(kStub, 1U, sizeof(kStub) - 1U, out);
  std::fclose(out);
  return written == (sizeof(kStub) - 1U);
}

} // namespace

/// Runs this executable or test program.
int main(int argc, char **argv) {
  record_arguments(argc, argv);
  return write_stub_output(argc, argv) ? 0 : 1;
}
