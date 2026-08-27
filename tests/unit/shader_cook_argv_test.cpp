// Regression for #333: the packer's shader cook launches shaderc as an
// argument vector, so manifest-authored strings are data to the child
// process and never text a shell parses. Drives the production packer CLI
// against a fake compiler that records its argument vector, over a manifest
// carrying the two authored fields that reach the launch: a variant define
// and a source path. Asserts that shell separators in a define are refused
// before any launch, that a source path built from shell substitution syntax
// arrives at the child byte-for-byte, and that neither payload runs.

#include "../test_harness.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using engine::tests::TestContext;

/// Quoted command-line fragment.
std::string quoted(const std::string &value) {
  return "\"" + value + "\"";
}

/// Points the fake compiler's argument recording at a path for the runs that
/// follow; the fake reads it from the environment it inherits.
void set_argv_log(const std::string &path) {
#ifdef _WIN32
  _putenv_s("ENGINE_FAKE_SHADERC_ARGV", path.c_str());
#else
  setenv("ENGINE_FAKE_SHADERC_ARGV", path.c_str(), 1);
#endif
}

/// Runs the packer's shader cook over one manifest with the fake compiler
/// standing in for shaderc; returns the packer's exit code.
int run_cook(const std::string &manifest, const std::string &outDir,
             const std::string &include) {
  std::string command = quoted(ENGINE_TEST_ASSET_PACKER) +
                        " --shader-manifest " + quoted(manifest) +
                        " --shader-out " + quoted(outDir) + " --shaderc " +
                        quoted(ENGINE_TEST_FAKE_SHADERC) +
                        " --shader-include " + quoted(include) +
                        " --profiles glsl";
#ifdef _WIN32
  // cmd.exe strips the outer quote pair from the whole command line.
  command = "\"" + command + "\"";
#endif
  return std::system(command.c_str());
}

/// Writes a file with the given text; false on any write failure.
bool write_text(const fs::path &path, const std::string &text) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    return false;
  }
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
  return file.good();
}

/// Reads the fake compiler's recording as one entry per line; empty when the
/// compiler was never launched.
std::vector<std::string> read_lines(const fs::path &path) {
  std::vector<std::string> lines;
  std::ifstream file(path, std::ios::binary);
  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty() && (line.back() == '\r')) {
      line.pop_back();
    }
    lines.push_back(line);
  }
  return lines;
}

/// Returns the argument recorded after the last occurrence of a flag, or an
/// empty string when the flag carried no value.
std::string value_after(const std::vector<std::string> &lines,
                        const std::string &flag) {
  for (std::size_t i = 0U; (i + 1U) < lines.size(); ++i) {
    if (lines[i] == flag) {
      return lines[i + 1U];
    }
  }
  return {};
}

/// Counts the recorded launches of the fake compiler.
std::size_t count_runs(const std::vector<std::string> &lines) {
  std::size_t runs = 0U;
  for (const std::string &line : lines) {
    if (line == "<run>") {
      ++runs;
    }
  }
  return runs;
}

/// Writes the two shaderc include headers the cook digests as inputs.
bool write_include_stubs(const fs::path &include) {
  return write_text(include / "bgfx_shader.sh", "// stub\n") &&
         write_text(include / "bgfx_compute.sh", "// stub\n");
}

/// Builds a one-entry manifest over one vertex source and one variant.
std::string manifest_text(const std::string &source,
                          const std::vector<std::string> &defines) {
  std::string defineList;
  for (std::size_t i = 0U; i < defines.size(); ++i) {
    if (i > 0U) {
      defineList += ", ";
    }
    defineList += "\"" + defines[i] + "\"";
  }
  return "{\n  \"shaders\": [\n    {\"source\": \"" + source +
         "\", \"type\": \"vertex\", \"output\": \"probe.vert\", "
         "\"variants\": [[" +
         defineList + "]]}\n  ]\n}\n";
}

} // namespace

/// Runs this executable or test program.
int main() {
  TestContext t{};

  const fs::path scratch =
      fs::temp_directory_path() / "engine_shader_cook_argv_test";
  std::error_code ignored;
  fs::remove_all(scratch, ignored);
  const fs::path include = scratch / "include";
  fs::create_directories(include, ignored);

  // The payloads below spawn their command in the packer's working
  // directory, so the test watches for them from there.
  const fs::path savedCwd = fs::current_path();
  std::error_code cwdError{};
  fs::current_path(scratch, cwdError);
  if (cwdError) {
    t.fail("scratch working directory");
    return t.finish("shader_cook_argv");
  }

  if (!write_include_stubs(include)) {
    t.fail("cook include stubs written");
    fs::current_path(savedCwd, ignored);
    return t.finish("shader_cook_argv");
  }

  // --- A define carrying shell separators is refused before any launch. ---
  {
    const fs::path sources = scratch / "sources_define";
    fs::create_directories(sources, ignored);
    const fs::path sentinel = scratch / "define_payload_ran";
    const fs::path argvLog = scratch / "define_argv.txt";
    set_argv_log(argvLog.string());

    // Each host's shell has its own statement separator, so the payload is
    // the one that host's shell would act on: a second command creating the
    // sentinel. A launch that reaches no shell leaves it as text.
#ifdef _WIN32
    const std::string define = "INJECT& type nul > define_payload_ran";
#else
    const std::string define = "INJECT; touch define_payload_ran";
#endif
    if (!write_text(sources / "varying.def.sc",
                    "vec4 v_color : COLOR0;\n") ||
        !write_text(sources / "probe.vs.sc", "// stub source\n") ||
        !write_text(sources / "shaders.json",
                    manifest_text("probe.vs.sc", {define}))) {
      t.fail("define-case manifest written");
      fs::current_path(savedCwd, ignored);
      return t.finish("shader_cook_argv");
    }

    const int exitCode = run_cook((sources / "shaders.json").string(),
                                  (scratch / "out_define").string(),
                                  include.string());
    t.check(exitCode != 0, "a define outside the macro grammar fails the cook");
    t.check(!fs::exists(sentinel),
            "the define's second command never ran");
    t.check(!fs::exists(argvLog),
            "a refused define never reaches the compiler");
    t.check(!fs::exists(scratch / "out_define" / "bgfx_shaders.cookstamp"),
            "a refused manifest certifies no cook");
  }

  // --- A source path built from substitution syntax arrives verbatim. ---
  {
    const fs::path sources = scratch / "sources_path";
    fs::create_directories(sources, ignored);
    // Both hosts' expansions are present: POSIX command substitution, whose
    // payload creates the sentinel, and a cmd.exe variable reference, whose
    // expansion would rewrite the path the compiler receives. A filename is
    // authored data the define grammar does not constrain, so this is what
    // proves the launch itself carries strings rather than parsing them.
    const std::string sourceName = "probe$(touch path_payload_ran)%PATH%.vs.sc";
    const fs::path sentinel = scratch / "path_payload_ran";
    const fs::path argvLog = scratch / "path_argv.txt";
    set_argv_log(argvLog.string());

    if (!write_text(sources / "varying.def.sc",
                    "vec4 v_color : COLOR0;\n") ||
        !write_text(sources / sourceName, "// stub source\n") ||
        !write_text(sources / "shaders.json",
                    manifest_text(sourceName, {"PBR_FULL"}))) {
      t.fail("path-case manifest written");
      fs::current_path(savedCwd, ignored);
      return t.finish("shader_cook_argv");
    }

    const int exitCode = run_cook((sources / "shaders.json").string(),
                                  (scratch / "out_path").string(),
                                  include.string());
    t.check(exitCode == 0, "a cook over that source succeeds");
    t.check(!fs::exists(sentinel), "the path's substitution never ran");

    const std::vector<std::string> lines = read_lines(argvLog);
    t.check(count_runs(lines) == 1U, "the compiler was launched once");
    t.check(value_after(lines, "-f") == (sources / sourceName).string(),
            "the source path arrives as one verbatim argument");
    t.check(value_after(lines, "--define") == "PBR_FULL",
            "the variant define arrives as one verbatim argument");
    t.check(fs::exists(scratch / "out_path" / "probe.vert.PBR_FULL.glsl.bin"),
            "the cooked output commits");
  }

  fs::current_path(savedCwd, ignored);
  fs::remove_all(scratch, ignored);
  return t.finish("shader_cook_argv");
}
