// Regression for #331: shader manifest source and output entries are
// authored data, so the cook holds each to one plain filename component
// before composing any path from it. Drives the production packer CLI
// against the recording fake compiler over manifests whose entries carry
// traversal segments, absolute paths, backslash separators, drive
// designators, and empty strings, and asserts each is refused before any
// compiler launch, source read, or output write — a sentinel outside the
// output root stays untouched and no cook stamp certifies the refused
// manifest. One well-formed manifest pins the accept side so the
// validation cannot silently over-reject.

#include "../test_harness.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;
using engine::tests::TestContext;

/// Quoted command-line fragment.
std::string quoted(const std::string &value) {
  return "\"" + value + "\"";
}

/// Points the fake compiler's argument recording at a path for the runs
/// that follow; the fake reads it from the environment it inherits. An
/// absent recording file after a cook proves the compiler never launched.
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

/// Reads a whole file as text; empty when unreadable.
std::string read_text(const fs::path &path) {
  std::ifstream file(path, std::ios::binary);
  std::string text;
  char c = '\0';
  while (file.get(c)) {
    text.push_back(c);
  }
  return text;
}

/// JSON-escapes an entry value so backslashes survive the manifest text.
std::string json_escaped(const std::string &value) {
  std::string escaped;
  for (const char c : value) {
    if ((c == '\\') || (c == '"')) {
      escaped.push_back('\\');
    }
    escaped.push_back(c);
  }
  return escaped;
}

/// Builds a one-entry manifest over one vertex source and one variant.
std::string manifest_text(const std::string &source,
                          const std::string &output) {
  return "{\n  \"shaders\": [\n    {\"source\": \"" + json_escaped(source) +
         "\", \"type\": \"vertex\", \"output\": \"" + json_escaped(output) +
         "\", \"variants\": [[]]}\n  ]\n}\n";
}

/// Writes the varying table, a stub source, and the manifest into one
/// fresh source directory; false on any write failure.
bool write_source_dir(const fs::path &sources, const std::string &source,
                      const std::string &output) {
  std::error_code ignored;
  fs::create_directories(sources, ignored);
  return write_text(sources / "varying.def.sc", "vec4 v_color : COLOR0;\n") &&
         write_text(sources / "probe.vs.sc", "// stub source\n") &&
         write_text(sources / "shaders.json", manifest_text(source, output));
}

/// Asserts one refused manifest: nonzero exit, no compiler launch, and no
/// cook stamp under the output directory.
void check_refused(TestContext &t, const fs::path &scratch,
                   const fs::path &include, const std::string &label,
                   const std::string &source, const std::string &output) {
  const fs::path sources = scratch / ("sources_" + label);
  const fs::path outDir = scratch / ("out_" + label);
  const fs::path argvLog = scratch / (label + "_argv.txt");
  set_argv_log(argvLog.string());
  if (!write_source_dir(sources, source, output)) {
    t.fail((label + ": manifest written").c_str());
    return;
  }
  const int exitCode = run_cook((sources / "shaders.json").string(),
                                outDir.string(), include.string());
  t.check(exitCode != 0, (label + ": the cook refuses the manifest").c_str());
  t.check(!fs::exists(argvLog),
          (label + ": the compiler never launched").c_str());
  t.check(!fs::exists(outDir / "bgfx_shaders.cookstamp"),
          (label + ": no stamp certifies the refused manifest").c_str());
}

} // namespace

/// Runs this executable or test program.
int main() {
  TestContext t{};

  const fs::path scratch =
      fs::temp_directory_path() / "engine_shader_cook_paths_test";
  std::error_code ignored;
  fs::remove_all(scratch, ignored);
  fs::create_directories(scratch / "include", ignored);
  const fs::path include = scratch / "include";
  if (!write_text(include / "bgfx_shader.sh", "// stub\n") ||
      !write_text(include / "bgfx_compute.sh", "// stub\n")) {
    t.fail("cook include stubs written");
    return t.finish("shader_cook_paths");
  }

  // --- A traversing output cannot write outside the output root. ---
  {
    const fs::path sources = scratch / "sources_out_traverse";
    const fs::path outDir = scratch / "out_traverse" / "cooked";
    // The escape target the traversal addresses; it exists so the base
    // behavior (committing the binary there) has nothing to trip over.
    fs::create_directories(scratch / "out_traverse" / "escape", ignored);
    const fs::path escaped = scratch / "out_traverse" / "escape" /
                             "owned.vert.default.glsl.bin";
    const fs::path argvLog = scratch / "out_traverse_argv.txt";
    set_argv_log(argvLog.string());
    if (!write_source_dir(sources, "probe.vs.sc", "../escape/owned.vert")) {
      t.fail("traversing-output manifest written");
      return t.finish("shader_cook_paths");
    }
    const int exitCode = run_cook((sources / "shaders.json").string(),
                                  outDir.string(), include.string());
    t.check(exitCode != 0, "a traversing output fails the cook");
    t.check(!fs::exists(argvLog),
            "a traversing output never reaches the compiler");
    t.check(!fs::exists(escaped),
            "no binary lands outside the output root");
  }

  // --- An absolute output cannot replace the output root. ---
  {
    const fs::path sources = scratch / "sources_out_absolute";
    const fs::path outDir = scratch / "out_absolute";
    fs::create_directories(scratch / "elsewhere", ignored);
    const fs::path target = scratch / "elsewhere" / "owned.vert";
    const fs::path escaped = scratch / "elsewhere" /
                             "owned.vert.default.glsl.bin";
    // A pre-existing file at the escape destination, so an escape that
    // overwrites it is distinguishable from one that merely creates it.
    if (!write_text(escaped, "authored bytes\n")) {
      t.fail("absolute-output sentinel written");
      return t.finish("shader_cook_paths");
    }
    const fs::path argvLog = scratch / "out_absolute_argv.txt";
    set_argv_log(argvLog.string());
    if (!write_source_dir(sources, "probe.vs.sc", target.string())) {
      t.fail("absolute-output manifest written");
      return t.finish("shader_cook_paths");
    }
    const int exitCode = run_cook((sources / "shaders.json").string(),
                                  outDir.string(), include.string());
    t.check(exitCode != 0, "an absolute output fails the cook");
    t.check(!fs::exists(argvLog),
            "an absolute output never reaches the compiler");
    t.check(read_text(escaped) == "authored bytes\n",
            "the file at the escape destination keeps its bytes");
  }

  // --- A traversing source cannot read outside the manifest's directory. ---
  {
    // A readable file above the manifest directory; the traversal names it,
    // so the base behavior (digesting and cooking it) succeeds and the
    // refusal is what changes the outcome.
    if (!write_text(scratch / "outside.vs.sc", "// outside source\n")) {
      t.fail("traversing-source outside file written");
      return t.finish("shader_cook_paths");
    }
    check_refused(t, scratch, include, "src_traverse", "../outside.vs.sc",
                  "probe.vert");
  }

  // --- An absolute source is refused. ---
  check_refused(t, scratch, include, "src_absolute",
                (scratch / "outside.vs.sc").string(), "probe.vert");

  // --- A backslash separator is refused on every host. ---
  check_refused(t, scratch, include, "out_backslash", "probe.vs.sc",
                "..\\escape\\owned.vert");

  // --- A drive designator is refused on every host. ---
  check_refused(t, scratch, include, "out_drive", "probe.vs.sc",
                "C:owned.vert");

  // --- An empty entry is refused. ---
  check_refused(t, scratch, include, "out_empty", "probe.vs.sc", "");
  check_refused(t, scratch, include, "src_empty", "", "probe.vert");

  // --- A plain filename still cooks: the validation over-rejects nothing. ---
  {
    const fs::path sources = scratch / "sources_plain";
    const fs::path outDir = scratch / "out_plain";
    const fs::path argvLog = scratch / "plain_argv.txt";
    set_argv_log(argvLog.string());
    if (!write_source_dir(sources, "probe.vs.sc", "probe.vert")) {
      t.fail("plain manifest written");
      return t.finish("shader_cook_paths");
    }
    const int exitCode = run_cook((sources / "shaders.json").string(),
                                  outDir.string(), include.string());
    t.check(exitCode == 0, "a plain-filename manifest cooks");
    t.check(fs::exists(outDir / "probe.vert.default.glsl.bin"),
            "the cooked output commits inside the output root");
  }

  fs::remove_all(scratch, ignored);
  return t.finish("shader_cook_paths");
}
