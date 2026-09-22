// The shader cook commits its outputs as one set (#547). It used to write
// each binary over its predecessor as soon as that one compiled, so a cook
// that failed partway -- a compile error in the second of two shaders --
// left the new first binary beside the old second one. A vertex stage from
// one generation linked against a fragment stage from another gives a
// varying mismatch or corrupted rendering, and nothing names the cause.
//
// Drives the production packer CLI with the fake compiler, whose output
// carries the source text so the generation of every binary is readable.

#include "../test_harness.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

namespace fs = std::filesystem;
using engine::tests::TestContext;

std::string quoted(const std::string &value) { return "\"" + value + "\""; }

bool write_text(const fs::path &path, const std::string &text) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    return false;
  }
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
  return file.good();
}

std::string read_text(const fs::path &path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file),
                     std::istreambuf_iterator<char>());
}

int run_cook(const fs::path &manifest, const fs::path &outDir,
             const fs::path &include) {
  std::string command =
      quoted(ENGINE_TEST_ASSET_PACKER) + " --shader-manifest " +
      quoted(manifest.string()) + " --shader-out " + quoted(outDir.string()) +
      " --shaderc " + quoted(ENGINE_TEST_FAKE_SHADERC) + " --shader-include " +
      quoted(include.string()) + " --profiles glsl";
#ifdef _WIN32
  // cmd.exe strips the outer quote pair from the whole command line.
  command = "\"" + command + "\"";
#endif
  return std::system(command.c_str());
}

/// Writes one generation of the two sources: a vertex and a fragment stage.
bool write_generation(const fs::path &sources, const std::string &tag,
                      bool breakFragment) {
  return write_text(sources / "probe.vs.sc", "// vertex " + tag + "\n") &&
         write_text(sources / "probe.fs.sc",
                    "// fragment " + tag + "\n" +
                        (breakFragment ? "FAKE_SHADERC_FAIL\n" : ""));
}

} // namespace

/// Runs this executable or test program.
int main() {
  TestContext t{};
  const fs::path scratch =
      fs::temp_directory_path() / "engine_shader_cook_transaction_test";
  std::error_code ignored;
  fs::remove_all(scratch, ignored);
  const fs::path include = scratch / "include";
  const fs::path sources = scratch / "sources";
  const fs::path out = scratch / "out";
  fs::create_directories(include, ignored);
  fs::create_directories(sources, ignored);

  const bool setUp =
      write_text(include / "bgfx_shader.sh", "// stub\n") &&
      write_text(include / "bgfx_compute.sh", "// stub\n") &&
      write_text(sources / "varying.def.sc", "vec4 v_color : COLOR0;\n") &&
      write_text(sources / "shaders.manifest",
                 "{\n  \"shaders\": [\n"
                 "    {\"source\": \"probe.vs.sc\", \"type\": \"vertex\", "
                 "\"output\": \"probe.vert\", \"variants\": [[]]},\n"
                 "    {\"source\": \"probe.fs.sc\", \"type\": \"fragment\", "
                 "\"output\": \"probe.frag\", \"variants\": [[]]}\n"
                 "  ]\n}\n") &&
      write_generation(sources, "one", false);
  t.check(setUp, "scratch sources written");
  if (!setUp) {
    return t.finish("shader_cook_transaction");
  }

  const fs::path vertex = out / "probe.vert.default.glsl.bin";
  const fs::path fragment = out / "probe.frag.default.glsl.bin";
  const fs::path stamp = out / "bgfx_shaders.cookstamp";

  t.check(run_cook(sources / "shaders.manifest", out, include) == 0,
          "generation one cooks");
  const std::string vertexOne = read_text(vertex);
  const std::string fragmentOne = read_text(fragment);
  const std::string stampOne = read_text(stamp);
  t.check(vertexOne.find("vertex one") != std::string::npos,
          "generation one's vertex binary is committed");
  t.check(fragmentOne.find("fragment one") != std::string::npos,
          "generation one's fragment binary is committed");

  // Generation two: the vertex stage compiles, the fragment stage does not.
  t.check(write_generation(sources, "two", true), "generation two written");
  t.check(run_cook(sources / "shaders.manifest", out, include) != 0,
          "a compile error fails the cook");
  t.check(read_text(vertex) == vertexOne,
          "the vertex binary is still generation one's: nothing is "
          "committed until every output has compiled");
  t.check(read_text(fragment) == fragmentOne,
          "the fragment binary is still generation one's");
  t.check(read_text(stamp) == stampOne, "the stamp still certifies it");
  bool strayStage = false;
  for (const fs::directory_entry &entry : fs::directory_iterator(out)) {
    const std::string name = entry.path().filename().string();
    strayStage = strayStage || (name.find(".cooking") != std::string::npos) ||
                 (name.find(".next") != std::string::npos);
  }
  t.check(!strayStage, "a failed cook leaves no staged file behind");

  // Fixed: generation two commits whole.
  t.check(write_generation(sources, "two", false), "generation two fixed");
  t.check(run_cook(sources / "shaders.manifest", out, include) == 0,
          "the fixed generation cooks");
  t.check((read_text(vertex).find("vertex two") != std::string::npos) &&
              (read_text(fragment).find("fragment two") != std::string::npos),
          "both binaries are generation two's");

  fs::remove_all(scratch, ignored);
  return t.finish("shader_cook_transaction");
}
