// Verifies the run log file core logging keeps beside stdout
// (log_open_file): every line lands in it, an Error is flushed to disk at
// once, reopening keeps the previous run's file under its previous-run
// name, a path that cannot be opened is refused with lines still reaching
// stdout, and the path of the last file opened outlives its closing so a
// failed start can still name it.

#include "engine/core/logging.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#include "../test_harness.h"

namespace {

std::string read_all(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  std::ostringstream text;
  text << stream.rdbuf();
  return text.str();
}

} // namespace

int main() {
  engine::tests::TestContext t;
  std::error_code ec{};
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path(ec) / "engine_log_file_test";
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  const std::string path = (dir / "editor.log").string();
  const std::string previous = (dir / "editor-prev.log").string();

  t.check(engine::core::initialize_logging(), "initialize logging");
  t.check(engine::core::log_file_path()[0] == '\0',
          "no log file before one is opened");

  // --- First run: lines land, an Error is on disk before the file closes.
  t.check(engine::core::log_open_file(path.c_str(), previous.c_str()),
          "open the log file");
  engine::core::log_message(engine::core::LogLevel::Info, "probe",
                            "first run info");
  engine::core::log_message(engine::core::LogLevel::Error, "probe",
                            "first run error");
  const std::string flushed = read_all(path);
  t.check((flushed.find("[Info][probe] first run info") != std::string::npos) &&
              (flushed.find("[Error][probe] first run error") !=
               std::string::npos),
          "an Error flushes every line before it to disk at once");
  engine::core::log_close_file();
  t.check(std::strcmp(engine::core::log_file_path(), path.c_str()) == 0,
          "the path outlives the file closing");

  // --- Second run: the first run's log is kept under its previous name.
  t.check(engine::core::log_open_file(path.c_str(), previous.c_str()),
          "reopen the log file");
  engine::core::log_message(engine::core::LogLevel::Warning, "probe",
                            "second run warning");
  engine::core::log_close_file();
  const std::string current = read_all(path);
  const std::string kept = read_all(previous);
  t.check((current.find("second run warning") != std::string::npos) &&
              (current.find("first run") == std::string::npos),
          "each run starts its own file");
  t.check(kept.find("first run error") != std::string::npos,
          "the previous run's log is kept beside it");

  // --- A path that cannot be opened is refused; logging goes on.
  const std::string unopenable = (dir / "missing" / "editor.log").string();
  const std::string unopenablePrev =
      (dir / "missing" / "editor-prev.log").string();
  t.check(!engine::core::log_open_file(unopenable.c_str(),
                                       unopenablePrev.c_str()),
          "a log file in a missing directory is refused");
  t.check(!engine::core::log_open_file("", previous.c_str()) &&
              !engine::core::log_open_file(nullptr, previous.c_str()) &&
              !engine::core::log_open_file(path.c_str(), nullptr),
          "an empty or null path is refused");
  engine::core::log_message(engine::core::LogLevel::Info, "probe",
                            "still logging to stdout");

  // --- Shutdown closes an open file.
  t.check(engine::core::log_open_file(path.c_str(), previous.c_str()),
          "open once more");
  engine::core::log_message(engine::core::LogLevel::Info, "probe",
                            "closed by shutdown");
  engine::core::shutdown_logging();
  t.check(read_all(path).find("closed by shutdown") != std::string::npos,
          "shutdown_logging flushes and closes the file");

  std::filesystem::remove_all(dir, ec);
  return t.finish("log_file");
}
