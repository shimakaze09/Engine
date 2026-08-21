// Verifies the packer's bgfx shader cook (#138 Phase C) through the
// production CLI: a full cook commits every manifest output plus the
// cook stamp, an unchanged re-run skips as up to date, two independent
// cooks are byte-identical (determinism), a source edit recooks through
// the stamp, and a failing shaderc leaves no stamp so a partial
// generation is never certified.

#include "../test_harness.h"

#include <cstdint>
#include <cstdio>
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

/// Runs the packer's shader cook against a manifest; returns exit code.
int run_cook(const std::string &packer, const std::string &manifest,
             const std::string &outDir, const std::string &shaderc,
             const std::string &include) {
  const std::string command =
      quoted(packer) + " --shader-manifest " + quoted(manifest) +
      " --shader-out " + quoted(outDir) + " --shaderc " + quoted(shaderc) +
      " --shader-include " + quoted(include) + " --profiles glsl,spirv";
  return std::system(command.c_str());
}

/// Whole-file byte read; empty on failure.
std::vector<char> read_file(const fs::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }
  return std::vector<char>((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
}

/// Copies the shader source directory into a scratch copy the test may
/// mutate.
bool copy_sources(const fs::path &from, const fs::path &to) {
  std::error_code error;
  fs::create_directories(to, error);
  fs::copy(from, to,
           fs::copy_options::overwrite_existing |
               fs::copy_options::recursive,
           error);
  return !error;
}

} // namespace

int main() {
  TestContext t{};
  const std::string packer = ENGINE_TEST_ASSET_PACKER;
  const std::string shaderc = ENGINE_TEST_SHADERC;
  const std::string include = ENGINE_TEST_SHADER_INCLUDE;
  const fs::path sourceDir = ENGINE_TEST_SHADER_SOURCE_DIR;

  const fs::path scratch =
      fs::temp_directory_path() / "engine_shader_cook_test";
  std::error_code ignored;
  fs::remove_all(scratch, ignored);
  const fs::path sources = scratch / "sources";
  const fs::path outA = scratch / "outA";
  const fs::path outB = scratch / "outB";
  if (!copy_sources(sourceDir, sources)) {
    t.fail("copy shader sources to scratch");
    return t.finish("shader_cook");
  }
  const std::string manifest = (sources / "shaders.json").string();

  // Full cook commits outputs and the stamp.
  t.check(run_cook(packer, manifest, outA.string(), shaderc, include) == 0,
          "initial cook succeeds");
  const fs::path sample = outA / "debug_line.vert.default.glsl.bin";
  const fs::path stamp = outA / "bgfx_shaders.cookstamp";
  t.check(fs::exists(sample), "cooked binary present");
  t.check(fs::exists(outA / "tonemap.frag.default.spirv.bin"),
          "second profile present");
  t.check(fs::exists(stamp), "cook stamp committed");
  const std::vector<char> firstBytes = read_file(sample);
  t.check(!firstBytes.empty(), "cooked binary non-empty");

  // Unchanged re-run: stamp-driven skip, bytes untouched.
  t.check(run_cook(packer, manifest, outA.string(), shaderc, include) == 0,
          "unchanged re-run succeeds");
  t.check(read_file(sample) == firstBytes, "skip left bytes untouched");

  // Determinism: an independent cook produces identical bytes.
  t.check(run_cook(packer, manifest, outB.string(), shaderc, include) == 0,
          "independent cook succeeds");
  t.check(read_file(outB / "debug_line.vert.default.glsl.bin") == firstBytes,
          "independent cook is byte-identical");

  // A source edit invalidates the stamp and recooks. Comments are
  // stripped at compile, so the observable is the rewritten stamp (its
  // dependency digests change), not the output bytes.
  const std::vector<char> firstStamp = read_file(stamp);
  {
    std::ofstream file(sources / "debug_line.vs.sc", std::ios::app);
    file << "\n// recook probe\n";
  }
  t.check(run_cook(packer, manifest, outA.string(), shaderc, include) == 0,
          "recook after source edit succeeds");
  t.check(read_file(stamp) != firstStamp,
          "edited source re-certified through a fresh stamp");
  t.check(!read_file(sample).empty(), "recooked binary present");

  // A failing tool must not certify a partial generation: no stamp.
  const fs::path outC = scratch / "outC";
  t.check(run_cook(packer, manifest, outC.string(),
                   (scratch / "missing-shaderc").string(), include) != 0,
          "missing shaderc fails the cook");
  t.check(!fs::exists(outC / "bgfx_shaders.cookstamp"),
          "failed cook leaves no stamp");

  fs::remove_all(scratch, ignored);
  return t.finish("shader_cook");
}
