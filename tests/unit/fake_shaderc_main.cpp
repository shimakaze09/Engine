// Stands in for bgfx shaderc so the packer's shader-cook invocation can be
// observed without a real shader compiler: it records the argument vector it
// was launched with and writes stub bytes to the output path the cook asked
// for, so a cook driven against it reaches its commit stage. The recording is
// what the argv suite asserts on - one line per argument, so an argument that
// was split or rewritten on its way to the child is visible as a line count
// or a line body that does not match what the manifest authored.

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace {

/// Reads the recording file's path from the ENGINE_FAKE_SHADERC_ARGV
/// environment variable; empty when the variable is unset. Windows hosts read
/// it through the bounds-checked form their CRT offers, since the plain one
/// is deprecated there.
std::string argv_log_path() {
#ifdef _WIN32
  char buffer[4096] = {};
  std::size_t length = 0U;
  if ((getenv_s(&length, buffer, sizeof(buffer),
                "ENGINE_FAKE_SHADERC_ARGV") != 0) ||
      (length == 0U)) {
    return {};
  }
  return buffer;
#else
  const char *value = std::getenv("ENGINE_FAKE_SHADERC_ARGV");
  return (value != nullptr) ? std::string(value) : std::string{};
#endif
}

/// Appends this run's argument vector to the recording file; a run with no
/// recording file configured records nothing.
void record_arguments(int argc, char **argv) {
  const std::string logPath = argv_log_path();
  if (logPath.empty()) {
    return;
  }
  std::ofstream log(logPath, std::ios::binary | std::ios::app);
  if (!log) {
    return;
  }
  // Runs append, so a cook that invoked the tool more than once is
  // distinguishable from one that invoked it once with more arguments.
  log << "<run>\n";
  for (int i = 1; i < argc; ++i) {
    log << argv[i] << "\n";
  }
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
  std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  out << "FAKE-SHADER-BINARY";
  return out.good();
}

} // namespace

/// Runs this executable or test program.
int main(int argc, char **argv) {
  record_arguments(argc, argv);
  return write_stub_output(argc, argv) ? 0 : 1;
}
