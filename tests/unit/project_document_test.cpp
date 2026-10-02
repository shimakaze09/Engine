// Verifies the project document through its production reader and
// writer:
// - a valid document round-trips, and formats to the same bytes each time,
//   pinned against the exact text;
// - every rule the schema states refuses its violation, naming the field
//   and leaving the caller's document untouched;
// - the scene list holds 1 to kMaxProjectScenes;
// - a truncated file, an absent file and an oversized file are each
//   reported as what they are;
// - the optional script limits read and write exactly, stay out of a
//   document that sets none, and are refused outside their range;
// - the optional package list reads and writes exactly, stays out of a
//   document that depends on nothing, and refuses a bad name, a repeat, a
//   source other than the embedded folder and a list past its limit;
// - the optional collision layers (names and ignored pairs) read in any
//   order and write in one canonical order, stay out of a document with
//   the default layers, and refuse a bad or repeated name, a repeated bit
//   or pair, a bit outside 0..31, an empty section and a one-sided matrix;
// - the optional save limit reads and writes exactly, stays out of a
//   document that sets none, and is refused outside 1..256 MiB.

#include "engine/content/project_document.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <string>

#include "../test_harness.h"

namespace {

namespace ct = engine::content;

constexpr const char *kGuid = "5fe40ece-6a1b-4c2d-9e3f-0a1b2c3d4e5f";

/// The exact text the writer produces for the reference document.
std::string reference_text() {
  return std::string("{\n"
                     "  \"schemaVersion\": 1,\n"
                     "  \"identity\": {\n"
                     "    \"name\": \"Island\",\n"
                     "    \"organisation\": \"Engine \\\"Samples\\\"\",\n"
                     "    \"version\": \"0.1.0\",\n"
                     "    \"guid\": \"") +
         kGuid +
         "\"\n"
         "  },\n"
         "  \"roots\": {\n"
         "    \"content\": \"assets\",\n"
         "    \"cache\": \".cache\"\n"
         "  },\n"
         "  \"scenes\": [\n"
         "    \"assets/coin_run.scene\",\n"
         "    \"assets/main.scene\"\n"
         "  ],\n"
         "  \"startupScene\": \"assets/coin_run.scene\",\n"
         "  \"mainScript\": \"assets/main.lua\"\n"
         "}\n";
}

std::string replaced(std::string text, const char *from, const char *to) {
  const std::size_t at = text.find(from);
  if (at != std::string::npos) {
    text.replace(at, std::strlen(from), to);
  }
  return text;
}

/// A document with `count` scenes, the first of them the startup scene.
std::string with_scenes(std::size_t count) {
  std::string list;
  for (std::size_t i = 0U; i < count; ++i) {
    char scene[64] = {};
    std::snprintf(scene, sizeof(scene), "%s\"assets/s%zu.scene\"",
                  (i == 0U) ? "" : ",", i);
    list += scene;
  }
  std::string text = reference_text();
  const std::size_t open = text.find("\"scenes\": [");
  const std::size_t close = text.find(']', open);
  text.replace(open, close - open + 1U, "\"scenes\": [" + list + "]");
  return replaced(text, "\"startupScene\": \"assets/coin_run.scene\"",
                  "\"startupScene\": \"assets/s0.scene\"");
}

std::unique_ptr<ct::ProjectDocument> fresh() {
  std::unique_ptr<ct::ProjectDocument> document(new (std::nothrow)
                                                    ct::ProjectDocument());
  if (document != nullptr) {
    std::snprintf(document->name, sizeof(document->name), "%s", "SENTINEL");
  }
  return document;
}

/// Parses `text` and checks it is refused as Malformed for `field`, with
/// the destination left as it was.
bool refused_for(const std::string &text, const char *field) {
  std::unique_ptr<ct::ProjectDocument> document = fresh();
  const auto result =
      ct::parse_project_document(text.data(), text.size(), document.get());
  const bool ok =
      !result.has_value() &&
      (result.error().kind == ct::ProjectReadFailureKind::Malformed) &&
      (std::strcmp(result.error().field, field) == 0) &&
      (std::strcmp(document->name, "SENTINEL") == 0);
  if (!ok) {
    std::fprintf(stderr, "  expected a refusal for '%s', got '%s' (%s)\n",
                 field, result.has_value() ? "accepted" : result.error().field,
                 result.has_value() ? "" : result.error().reason);
  }
  return ok;
}

void check_round_trip(engine::tests::TestContext &t) {
  const std::string text = reference_text();
  std::unique_ptr<ct::ProjectDocument> document = fresh();
  const auto parsed =
      ct::parse_project_document(text.data(), text.size(), document.get());
  t.check(parsed.has_value(), "the reference document parses");
  t.check(
      (std::strcmp(document->name, "Island") == 0) &&
          (std::strcmp(document->organisation, "Engine \"Samples\"") == 0) &&
          (std::strcmp(document->version, "0.1.0") == 0) &&
          (std::strcmp(document->contentRoot, "assets") == 0) &&
          (std::strcmp(document->cacheRoot, ".cache") == 0) &&
          (document->sceneCount == 2U) &&
          (std::strcmp(document->scenes[1], "assets/main.scene") == 0) &&
          (std::strcmp(document->startupScene, "assets/coin_run.scene") == 0) &&
          (std::strcmp(document->mainScript, "assets/main.lua") == 0),
      "every field reads back as written");

  std::unique_ptr<char[]> out(
      new (std::nothrow) char[ct::kMaxProjectDocumentBytes]);
  std::size_t length = 0U;
  t.check(ct::format_project_document(*document, out.get(),
                                      ct::kMaxProjectDocumentBytes, &length) &&
              (std::string(out.get(), length) == text),
          "the writer produces exactly the reference text");

  const std::string noScript =
      replaced(text, ",\n  \"mainScript\": \"assets/main.lua\"", "");
  std::unique_ptr<ct::ProjectDocument> bare = fresh();
  t.check(
      ct::parse_project_document(noScript.data(), noScript.size(), bare.get())
              .has_value() &&
          (bare->mainScript[0] == '\0') &&
          ct::format_project_document(*bare, out.get(),
                                      ct::kMaxProjectDocumentBytes, &length) &&
          (std::string(out.get(), length) == noScript),
      "a project without a main script omits the field both ways");

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "engine_project_test.project";
  std::unique_ptr<ct::ProjectDocument> fromFile = fresh();
  t.check(
      ct::write_project_document(path.string().c_str(), *document) &&
          ct::read_project_document(path.string().c_str(), fromFile.get())
              .has_value() &&
          (std::strcmp(fromFile->startupScene, "assets/coin_run.scene") == 0),
      "the file written reads back");
  std::ifstream written(path, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(written)),
                          std::istreambuf_iterator<char>());
  t.check(bytes == text, "the file holds exactly the reference text");

  // A refused write leaves the previous file exactly as it was.
  ct::ProjectDocument invalid = *document;
  invalid.name[0] = '\0';
  t.check(!ct::write_project_document(path.string().c_str(), invalid),
          "an invalid document is not written");
  std::ifstream after(path, std::ios::binary);
  const std::string kept((std::istreambuf_iterator<char>(after)),
                         std::istreambuf_iterator<char>());
  t.check(kept == text, "a refused write leaves the previous file intact");
  const std::filesystem::path missingDir =
      std::filesystem::temp_directory_path() / "engine_project_test_no_dir" /
      "x.project";
  t.check(!ct::write_project_document(missingDir.string().c_str(), *document) &&
              !std::filesystem::exists(missingDir),
          "a write into a missing directory fails and creates nothing");
  std::error_code ec{};
  std::filesystem::remove(path, ec);
}

void check_refusals(engine::tests::TestContext &t) {
  const std::string text = reference_text();
  struct Case final {
    const char *from;
    const char *to;
    const char *field;
  };
  const Case cases[] = {
      {"\"schemaVersion\": 1", "\"schemaVersion\": 2", "schemaVersion"},
      {"\"schemaVersion\": 1", "\"schemaVersion\": 0", "schemaVersion"},
      {"\"schemaVersion\": 1", "\"schemaVersion\": \"1\"", "schemaVersion"},
      {"\"schemaVersion\": 1,\n", "", "schemaVersion"},
      {"\"startupScene\"", "\"extra\": 1,\n  \"startupScene\"", "extra"},
      {"\"startupScene\"", "\"scenes\": [],\n  \"startupScene\"", "scenes"},
      {"\"version\": \"0.1.0\",",
       "\"version\": \"0.1.0\",\n    \"colour\": \"red\",", "identity.colour"},
      {"\"cache\": \".cache\"", "\"cache\": \".cache\",\n    \"x\": \"y\"",
       "roots.x"},
      {"\"name\": \"Island\",\n", "", "identity.name"},
      {"\"name\": \"Island\"", "\"name\": \"\"", "identity.name"},
      {"\"name\": \"Island\"", "\"name\": \"a/b\"", "identity.name"},
      {"\"name\": \"Island\"", "\"name\": \"Island.\"", "identity.name"},
      {"\"name\": \"Island\"", "\"name\": \"x\\ty\"", "identity.name"},
      {"\"name\": \"Island\"", "\"name\": 7", "identity.name"},
      {"\"name\": \"Island\"",
       "\"name\": "
       "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
       "\"",
       "identity.name"},
      {"\"version\": \"0.1.0\"", "\"version\": \"\"", "identity.version"},
      {kGuid, "00000000-0000-0000-0000-000000000000", "identity.guid"},
      {kGuid, "not-a-guid", "identity.guid"},
      {"\"content\": \"assets\"", "\"content\": \"/abs\"", "roots.content"},
      {"\"content\": \"assets\"", "\"content\": \"../up\"", "roots.content"},
      {"\"content\": \"assets\"", "\"content\": \"a\\\\b\"", "roots.content"},
      {"\"content\": \"assets\"", "\"content\": \"C:/x\"", "roots.content"},
      {"\"cache\": \".cache\"", "\"cache\": \"assets\"", "roots.cache"},
      {"\"cache\": \".cache\"", "\"cache\": \"assets/cache\"", "roots.cache"},
      {"\"roots\": {\n    \"content\": \"assets\",\n    \"cache\": "
       "\".cache\"\n  }",
       "\"roots\": 3", "roots"},
      {"\"assets/main.scene\"", "\"assets/coin_run.scene\"", "scenes[1]"},
      {"\"assets/main.scene\"", "\"other/main.scene\"", "scenes[1]"},
      {"\"assets/main.scene\"", "\"assets/main.lua\"", "scenes[1]"},
      {"\"assets/main.scene\"", "\"assets//main.scene\"", "scenes[1]"},
      {"\"assets/main.scene\"", "4", "scenes[1]"},
      {"\"startupScene\": \"assets/coin_run.scene\"",
       "\"startupScene\": \"assets/elsewhere.scene\"", "startupScene"},
      {"\"mainScript\": \"assets/main.lua\"", "\"mainScript\": \"\"",
       "mainScript"},
      {"\"mainScript\": \"assets/main.lua\"",
       "\"mainScript\": \"assets/main.txt\"", "mainScript"},
  };
  bool all = true;
  for (const Case &c : cases) {
    all = refused_for(replaced(text, c.from, c.to), c.field) && all;
  }
  t.check(all, "each rule refuses its violation, naming the field");

  const std::string twice =
      replaced(text, "\"startupScene\"",
               "\"mainScript\": \"assets/main.lua\",\n  \"startupScene\"");
  t.check(refused_for(twice, "mainScript"), "a repeated key is refused");
  t.check(refused_for(text.substr(0U, text.size() / 2U), ""),
          "a truncated document is refused");
  t.check(refused_for("[1,2]", ""), "a non-object document is refused");
}

void check_scene_bounds(engine::tests::TestContext &t) {
  std::unique_ptr<ct::ProjectDocument> document = fresh();
  const std::string one = with_scenes(1U);
  const std::string full = with_scenes(ct::kMaxProjectScenes);
  t.check(ct::parse_project_document(one.data(), one.size(), document.get())
                  .has_value() &&
              (document->sceneCount == 1U),
          "one scene is accepted");
  t.check(ct::parse_project_document(full.data(), full.size(), document.get())
                  .has_value() &&
              (document->sceneCount == ct::kMaxProjectScenes),
          "the most scenes a project holds are accepted");
  t.check(refused_for(with_scenes(0U), "scenes"), "no scenes are refused");
  t.check(refused_for(with_scenes(ct::kMaxProjectScenes + 1U), "scenes"),
          "one scene too many is refused, not cut short");
}

void check_file_outcomes(engine::tests::TestContext &t) {
  std::unique_ptr<ct::ProjectDocument> document = fresh();
  const std::filesystem::path absent =
      std::filesystem::temp_directory_path() / "engine_project_absent.project";
  std::error_code ec{};
  std::filesystem::remove(absent, ec);
  const auto missing =
      ct::read_project_document(absent.string().c_str(), document.get());
  t.check(!missing.has_value() &&
              (missing.error().kind == ct::ProjectReadFailureKind::Absent),
          "an absent file reads as Absent");

  const std::filesystem::path large =
      std::filesystem::temp_directory_path() / "engine_project_large.project";
  {
    std::ofstream out(large, std::ios::binary);
    const std::string padding(ct::kMaxProjectDocumentBytes + 16U, ' ');
    out << padding;
  }
  const auto tooLarge =
      ct::read_project_document(large.string().c_str(), document.get());
  t.check(!tooLarge.has_value() &&
              (tooLarge.error().kind == ct::ProjectReadFailureKind::TooLarge) &&
              (std::strcmp(document->name, "SENTINEL") == 0),
          "an oversized file reads as TooLarge and changes nothing");
  std::filesystem::remove(large, ec);
}

} // namespace

/// The reference document with `scripting` appended as its last member.
std::string with_scripting(const char *scripting) {
  return replaced(reference_text(), "\"assets/main.lua\"\n}",
                  (std::string("\"assets/main.lua\",\n  \"scripting\": ") +
                   scripting + "\n}")
                      .c_str());
}

void check_script_limits(engine::tests::TestContext &t) {
  std::unique_ptr<char[]> out(
      new (std::nothrow) char[ct::kMaxProjectDocumentBytes]);
  std::size_t length = 0U;

  const std::string plain = reference_text();
  std::unique_ptr<ct::ProjectDocument> none = fresh();
  t.check(ct::parse_project_document(plain.data(), plain.size(), none.get())
                  .has_value() &&
              !none->scriptLimits.instructionLimitSet &&
              !none->scriptLimits.memoryLimitSet,
          "a document with no scripting section sets no limit");

  const std::string both =
      with_scripting("{\n    \"instructionLimit\": 2500000,\n    "
                     "\"memoryLimitMiB\": 128\n  }");
  std::unique_ptr<ct::ProjectDocument> set = fresh();
  t.check(ct::parse_project_document(both.data(), both.size(), set.get())
                  .has_value() &&
              set->scriptLimits.instructionLimitSet &&
              (set->scriptLimits.instructionLimit == 2500000U) &&
              set->scriptLimits.memoryLimitSet &&
              (set->scriptLimits.memoryLimitMiB == 128U),
          "both limits read back as written");
  t.check(ct::format_project_document(*set, out.get(),
                                      ct::kMaxProjectDocumentBytes, &length) &&
              (std::string(out.get(), length) == both),
          "a document setting both limits writes exactly its text");

  const std::string memoryOnly =
      with_scripting("{\n    \"memoryLimitMiB\": 0\n  }");
  std::unique_ptr<ct::ProjectDocument> one = fresh();
  t.check(ct::parse_project_document(memoryOnly.data(), memoryOnly.size(),
                                     one.get())
                  .has_value() &&
              !one->scriptLimits.instructionLimitSet &&
              one->scriptLimits.memoryLimitSet &&
              (one->scriptLimits.memoryLimitMiB == 0U) &&
              ct::format_project_document(
                  *one, out.get(), ct::kMaxProjectDocumentBytes, &length) &&
              (std::string(out.get(), length) == memoryOnly),
          "one limit, 0 for unlimited, round-trips alone");

  struct Bound final {
    const char *scripting;
    bool accepted;
    const char *field;
  };
  const Bound bounds[] = {
      {"{\"instructionLimit\": 0}", true, ""},
      {"{\"instructionLimit\": 100000}", true, ""},
      {"{\"instructionLimit\": 99999}", false, "scripting.instructionLimit"},
      {"{\"instructionLimit\": 1}", false, "scripting.instructionLimit"},
      {"{\"instructionLimit\": 1000000000}", true, ""},
      {"{\"instructionLimit\": 1000000001}", false,
       "scripting.instructionLimit"},
      {"{\"memoryLimitMiB\": 16}", true, ""},
      {"{\"memoryLimitMiB\": 15}", false, "scripting.memoryLimitMiB"},
      {"{\"memoryLimitMiB\": 2048}", true, ""},
      {"{\"memoryLimitMiB\": 2049}", false, "scripting.memoryLimitMiB"},
  };
  for (const Bound &bound : bounds) {
    const std::string text = with_scripting(bound.scripting);
    std::unique_ptr<ct::ProjectDocument> document = fresh();
    const bool accepted =
        ct::parse_project_document(text.data(), text.size(), document.get())
            .has_value();
    char label[160] = {};
    std::snprintf(label, sizeof(label), "%s is %s", bound.scripting,
                  bound.accepted ? "accepted" : "refused");
    t.check(bound.accepted ? accepted : refused_for(text, bound.field), label);
  }

  t.check(refused_for(with_scripting("{}"), "scripting"),
          "an empty scripting section is refused; omit it instead");
  t.check(refused_for(with_scripting("5"), "scripting"),
          "a scripting section that is not an object is refused");
  t.check(
      refused_for(with_scripting("{\"cpuLimit\": 5}"), "scripting.cpuLimit"),
      "an unknown scripting key is refused");
  t.check(refused_for(with_scripting("{\"memoryLimitMiB\": 64, "
                                     "\"memoryLimitMiB\": 64}"),
                      "scripting.memoryLimitMiB"),
          "a repeated scripting key is refused");
  t.check(refused_for(with_scripting("{\"instructionLimit\": 150000.5}"),
                      "scripting.instructionLimit"),
          "a fractional limit is refused");
  t.check(refused_for(with_scripting("{\"instructionLimit\": \"150000\"}"),
                      "scripting.instructionLimit"),
          "a limit written as a string is refused");
  t.check(refused_for(with_scripting("{\"memoryLimitMiB\": -64}"),
                      "scripting.memoryLimitMiB"),
          "a negative limit is refused");
  t.check(refused_for(with_scripting("{\"memoryLimitMiB\": 4294967360}"),
                      "scripting.memoryLimitMiB"),
          "a limit past 32 bits is refused, not wrapped");

  std::unique_ptr<ct::ProjectDocument> invalid = fresh();
  t.check(ct::parse_project_document(plain.data(), plain.size(), invalid.get())
              .has_value(),
          "the reference parses for the writer check");
  invalid->scriptLimits.memoryLimitSet = true;
  invalid->scriptLimits.memoryLimitMiB = 8U;
  t.check(!ct::format_project_document(*invalid, out.get(),
                                       ct::kMaxProjectDocumentBytes, &length) &&
              (out[0] == '\0'),
          "the writer refuses a limit its reader would refuse");
}

/// The reference document with `saves` appended as its last member.
std::string with_saves(const char *saves) {
  return replaced(
      reference_text(), "\"assets/main.lua\"\n}",
      (std::string("\"assets/main.lua\",\n  \"saves\": ") + saves + "\n}")
          .c_str());
}

void check_save_settings(engine::tests::TestContext &t) {
  std::unique_ptr<char[]> out(
      new (std::nothrow) char[ct::kMaxProjectDocumentBytes]);
  std::size_t length = 0U;

  const std::string plain = reference_text();
  std::unique_ptr<ct::ProjectDocument> none = fresh();
  t.check(ct::parse_project_document(plain.data(), plain.size(), none.get())
                  .has_value() &&
              !none->saveSettings.maxSlotMiBSet &&
              ct::format_project_document(
                  *none, out.get(), ct::kMaxProjectDocumentBytes, &length) &&
              (std::string(out.get(), length) == plain),
          "a document with no saves section sets nothing and writes none");

  const std::string sixteen = with_saves("{\n    \"maxSlotMiB\": 16\n  }");
  std::unique_ptr<ct::ProjectDocument> set = fresh();
  t.check(ct::parse_project_document(sixteen.data(), sixteen.size(), set.get())
                  .has_value() &&
              set->saveSettings.maxSlotMiBSet &&
              (set->saveSettings.maxSlotMiB == 16U) &&
              ct::format_project_document(
                  *set, out.get(), ct::kMaxProjectDocumentBytes, &length) &&
              (std::string(out.get(), length) == sixteen),
          "a save limit reads back and writes exactly its text");

  struct Bound final {
    const char *saves;
    bool accepted;
    const char *field;
  };
  const Bound bounds[] = {
      {"{\"maxSlotMiB\": 1}", true, ""},
      {"{\"maxSlotMiB\": 0}", false, "saves.maxSlotMiB"},
      {"{\"maxSlotMiB\": 256}", true, ""},
      {"{\"maxSlotMiB\": 257}", false, "saves.maxSlotMiB"},
      {"{\"maxSlotMiB\": -1}", false, "saves.maxSlotMiB"},
      {"{\"maxSlotMiB\": 1.5}", false, "saves.maxSlotMiB"},
      {"{\"maxSlotMiB\": \"4\"}", false, "saves.maxSlotMiB"},
      {"{\"maxSlotMiB\": 4294967300}", false, "saves.maxSlotMiB"},
      {"{}", false, "saves"},
      {"[]", false, "saves"},
      {"{\"maxSlots\": 4}", false, "saves.maxSlots"},
      {"{\"maxSlotMiB\": 4, \"maxSlotMiB\": 4}", false, "saves.maxSlotMiB"},
  };
  for (const Bound &bound : bounds) {
    const std::string text = with_saves(bound.saves);
    std::unique_ptr<ct::ProjectDocument> document = fresh();
    const bool accepted =
        ct::parse_project_document(text.data(), text.size(), document.get())
            .has_value();
    char label[160] = {};
    std::snprintf(label, sizeof(label), "saves %s is %s", bound.saves,
                  bound.accepted ? "accepted" : "refused");
    t.check(bound.accepted ? accepted : refused_for(text, bound.field), label);
  }

  std::unique_ptr<ct::ProjectDocument> invalid = fresh();
  t.check(ct::parse_project_document(plain.data(), plain.size(), invalid.get())
              .has_value(),
          "the reference parses for the save writer check");
  invalid->saveSettings.maxSlotMiBSet = true;
  invalid->saveSettings.maxSlotMiB = 0U;
  t.check(!ct::format_project_document(*invalid, out.get(),
                                       ct::kMaxProjectDocumentBytes, &length) &&
              (out[0] == '\0'),
          "the writer refuses a save limit its reader would refuse");
}

/// The reference document with `dependencies` appended as its last member.
std::string with_dependencies(const char *dependencies) {
  return replaced(reference_text(), "\"assets/main.lua\"\n}",
                  (std::string("\"assets/main.lua\",\n  \"dependencies\": ") +
                   dependencies + "\n}")
                      .c_str());
}

void check_packages(engine::tests::TestContext &t) {
  std::unique_ptr<char[]> out(
      new (std::nothrow) char[ct::kMaxProjectDocumentBytes]);
  std::size_t length = 0U;

  const std::string plain = reference_text();
  std::unique_ptr<ct::ProjectDocument> none = fresh();
  t.check(ct::parse_project_document(plain.data(), plain.size(), none.get())
                  .has_value() &&
              (none->packageCount == 0U),
          "a document with no dependencies has no packages");

  const std::string two = with_dependencies(
      "[\n    {\"name\": \"input_system\", \"source\": "
      "\"packages/input_system\"},\n    {\"name\": \"text-mesh2\", "
      "\"source\": \"packages/text-mesh2\"}\n  ]");
  std::unique_ptr<ct::ProjectDocument> read = fresh();
  t.check(
      ct::parse_project_document(two.data(), two.size(), read.get())
              .has_value() &&
          (read->packageCount == 2U) &&
          (std::strcmp(read->packages[0].name, "input_system") == 0) &&
          (std::strcmp(read->packages[1].source, "packages/text-mesh2") == 0),
      "two packages read back in the author's order");
  t.check(ct::format_project_document(*read, out.get(),
                                      ct::kMaxProjectDocumentBytes, &length) &&
              (std::string(out.get(), length) == two),
          "a document with packages writes exactly its text");

  const auto package = [](const char *name, const char *source) {
    return std::string("[{\"name\": \"") + name + "\", \"source\": \"" +
           source + "\"}]";
  };
  t.check(refused_for(with_dependencies("[]"), "dependencies"),
          "an empty dependencies list is refused; omit it instead");
  t.check(refused_for(with_dependencies("{}"), "dependencies"),
          "dependencies that are not an array are refused");
  t.check(refused_for(with_dependencies("[5]"), "dependencies[0]"),
          "a package that is not an object is refused");
  t.check(refused_for(with_dependencies("[{\"name\": \"a\"}]"),
                      "dependencies[0].source"),
          "a package with no source is refused");
  t.check(refused_for(with_dependencies("[{\"name\": \"a\", \"source\": "
                                        "\"packages/a\", \"version\": 1}]"),
                      "dependencies[0].version"),
          "an unknown package key is refused");
  t.check(
      refused_for(with_dependencies(package("Input", "packages/Input").c_str()),
                  "dependencies[0].name"),
      "an upper-case package name is refused");
  t.check(refused_for(with_dependencies(package("-a", "packages/-a").c_str()),
                      "dependencies[0].name"),
          "a package name starting with '-' is refused");
  t.check(refused_for(with_dependencies(package("a b", "packages/a b").c_str()),
                      "dependencies[0].name"),
          "a package name holding a space is refused");
  t.check(refused_for(with_dependencies(package("", "packages/").c_str()),
                      "dependencies[0].name"),
          "an empty package name is refused");
  const std::string longName(ct::kProjectPackageNameCapacity, 'a');
  t.check(
      refused_for(with_dependencies(package(longName.c_str(),
                                            ("packages/" + longName).c_str())
                                        .c_str()),
                  "dependencies[0].name"),
      "a package name that does not fit is refused, not cut short");
  const std::string fits(ct::kProjectPackageNameCapacity - 1U, 'a');
  std::unique_ptr<ct::ProjectDocument> longest = fresh();
  const std::string fitsText = with_dependencies(
      package(fits.c_str(), ("packages/" + fits).c_str()).c_str());
  t.check(ct::parse_project_document(fitsText.data(), fitsText.size(),
                                     longest.get())
              .has_value(),
          "the longest package name that fits is accepted");
  t.check(refused_for(with_dependencies(package("a", "vendor/a").c_str()),
                      "dependencies[0].source"),
          "a source other than the embedded folder is refused");
  t.check(refused_for(with_dependencies(package("a", "packages/b").c_str()),
                      "dependencies[0].source"),
          "a source naming another package's folder is refused");
  t.check(refused_for(with_dependencies("[{\"name\": \"a\", \"source\": "
                                        "\"packages/a\"}, {\"name\": \"a\", "
                                        "\"source\": \"packages/a\"}]"),
                      "dependencies[1].name"),
          "a repeated package is refused");
  t.check(refused_for(
              replaced(with_dependencies(package("a", "packages/a").c_str()),
                       "\"content\": \"assets\"", "\"content\": \"packages\""),
              "dependencies[0].source"),
          "a package inside the content root is refused");

  std::string many = "[";
  for (std::size_t i = 0U; i <= ct::kMaxProjectPackages; ++i) {
    char name[16] = {};
    std::snprintf(name, sizeof(name), "p%zu", i);
    many += (i == 0U) ? "" : ",";
    many += "{\"name\": \"" + std::string(name) +
            "\", \"source\": \"packages/" + name + "\"}";
    if (i + 1U == ct::kMaxProjectPackages) {
      std::unique_ptr<ct::ProjectDocument> full = fresh();
      const std::string atLimit = with_dependencies((many + "]").c_str());
      t.check(
          ct::parse_project_document(atLimit.data(), atLimit.size(), full.get())
                  .has_value() &&
              (full->packageCount == ct::kMaxProjectPackages),
          "a project may depend on as many packages as it can hold");
    }
  }
  many += "]";
  t.check(refused_for(with_dependencies(many.c_str()), "dependencies"),
          "one package past the limit is refused");
}

/// The reference document with `physics` appended as its last member.
std::string with_physics(const char *physics) {
  return replaced(
      reference_text(), "\"assets/main.lua\"\n}",
      (std::string("\"assets/main.lua\",\n  \"physics\": ") + physics + "\n}")
          .c_str());
}

void check_collision_layers(engine::tests::TestContext &t) {
  std::unique_ptr<char[]> out(
      new (std::nothrow) char[ct::kMaxProjectDocumentBytes]);
  std::size_t length = 0U;

  const std::string plain = reference_text();
  std::unique_ptr<ct::ProjectDocument> none = fresh();
  t.check(ct::parse_project_document(plain.data(), plain.size(), none.get())
                  .has_value() &&
              ct::collision_layers_are_default(none->collisionLayers),
          "a document with no physics section has the default layers");

  const std::string canonical = with_physics(
      "{\n    \"layers\": [\n      {\"bit\": 0, \"name\": \"Default\"},\n"
      "      {\"bit\": 3, \"name\": \"Player\"},\n      {\"bit\": 31, "
      "\"name\": \"Enemy.Projectile\"}\n    ],\n    \"ignoredPairs\": [\n"
      "      [3, 3],\n      [3, 31]\n    ]\n  }");
  std::unique_ptr<ct::ProjectDocument> read = fresh();
  const bool parsed =
      ct::parse_project_document(canonical.data(), canonical.size(), read.get())
          .has_value();
  const ct::ProjectCollisionLayers &layers = read->collisionLayers;
  t.check(parsed && (std::strcmp(layers.names[3], "Player") == 0) &&
              (ct::find_collision_layer(layers, "player") == 3) &&
              (ct::find_collision_layer(layers, "ENEMY.projectile") == 31) &&
              (ct::find_collision_layer(layers, "Ghost") == -1) &&
              (ct::find_collision_layer(layers, "") == -1) &&
              (layers.names[1][0] == '\0'),
          "names read by bit and are found ignoring case");
  t.check(parsed && (layers.collides[3] == ~((1U << 3U) | (1U << 31U))) &&
              (layers.collides[31] == ~(1U << 3U)) &&
              (layers.collides[0] == 0xFFFFFFFFU),
          "ignored pairs clear both rows, a layer's pair with itself one bit");
  t.check(ct::format_project_document(*read, out.get(),
                                      ct::kMaxProjectDocumentBytes, &length) &&
              (std::string(out.get(), length) == canonical),
          "named layers and ignored pairs write exactly their canonical text");

  // A layer named in the author's own script (地面, "ground") reads and
  // writes back byte for byte (#1185).
  const std::string cjkLayers =
      with_physics("{\n    \"layers\": [\n      {\"bit\": 4, \"name\": "
                   "\"\xE5\x9C\xB0\xE9\x9D\xA2\"}\n    ]\n  }");
  std::unique_ptr<ct::ProjectDocument> cjk = fresh();
  t.check(
      ct::parse_project_document(cjkLayers.data(), cjkLayers.size(), cjk.get())
              .has_value() &&
          (ct::find_collision_layer(cjk->collisionLayers,
                                    "\xE5\x9C\xB0\xE9\x9D\xA2") == 4) &&
          ct::format_project_document(*cjk, out.get(),
                                      ct::kMaxProjectDocumentBytes, &length) &&
          (std::string(out.get(), length) == cjkLayers),
      "a CJK layer name reads and writes back byte for byte");

  const std::string shuffled = with_physics(
      "{\"ignoredPairs\": [[31, 3], [3, 3]], \"layers\": [{\"name\": "
      "\"Enemy.Projectile\", \"bit\": 31}, {\"bit\": 3, \"name\": "
      "\"Player\"}, {\"bit\": 0, \"name\": \"Default\"}]}");
  std::unique_ptr<ct::ProjectDocument> reordered = fresh();
  t.check(ct::parse_project_document(shuffled.data(), shuffled.size(),
                                     reordered.get())
                  .has_value() &&
              ct::format_project_document(*reordered, out.get(),
                                          ct::kMaxProjectDocumentBytes,
                                          &length) &&
              (std::string(out.get(), length) == canonical),
          "any order reads, and writes back in the one canonical order");

  const std::string namesOnly = with_physics(
      "{\n    \"layers\": [\n      {\"bit\": 5, \"name\": \"Water\"}\n    "
      "]\n  }");
  const std::string pairsOnly =
      with_physics("{\n    \"ignoredPairs\": [\n      [0, 1]\n    ]\n  }");
  for (const std::string *text : {&namesOnly, &pairsOnly}) {
    std::unique_ptr<ct::ProjectDocument> one = fresh();
    t.check(ct::parse_project_document(text->data(), text->size(), one.get())
                    .has_value() &&
                ct::format_project_document(
                    *one, out.get(), ct::kMaxProjectDocumentBytes, &length) &&
                (std::string(out.get(), length) == *text),
            "names alone and pairs alone each round-trip");
  }

  std::string all = "{\"layers\": [";
  for (int bit = 0; bit < 32; ++bit) {
    char entry[48] = {};
    std::snprintf(entry, sizeof(entry), "%s{\"bit\": %d, \"name\": \"L%d\"}",
                  (bit == 0) ? "" : ",", bit, bit);
    all += entry;
  }
  std::unique_ptr<ct::ProjectDocument> full = fresh();
  const std::string thirtyTwo = with_physics((all + "]}").c_str());
  t.check(
      ct::parse_project_document(thirtyTwo.data(), thirtyTwo.size(), full.get())
              .has_value() &&
          (ct::find_collision_layer(full->collisionLayers, "L31") == 31),
      "all 32 layers may be named");
  t.check(refused_for(with_physics((all + ",{\"bit\": 0, \"name\": "
                                          "\"Extra\"}]}")
                                       .c_str()),
                      "physics.layers"),
          "a 33rd layer entry is refused");

  struct Refusal final {
    const char *physics;
    const char *field;
    const char *what;
  };
  const Refusal refusals[] = {
      {"{}", "physics", "an empty physics section"},
      {"[]", "physics", "a physics section that is not an object"},
      {"{\"gravity\": 1}", "physics.gravity", "an unknown physics key"},
      {"{\"layers\": []}", "physics.layers", "an empty layer list"},
      {"{\"ignoredPairs\": []}", "physics.ignoredPairs", "an empty pair list"},
      {"{\"layers\": [{\"bit\": 32, \"name\": \"Far\"}]}",
       "physics.layers[0].bit", "a bit past 31"},
      {"{\"layers\": [{\"bit\": -1, \"name\": \"Neg\"}]}",
       "physics.layers[0].bit", "a negative bit"},
      {"{\"layers\": [{\"bit\": 1.5, \"name\": \"Half\"}]}",
       "physics.layers[0].bit", "a fractional bit"},
      {"{\"layers\": [{\"bit\": 1}]}", "physics.layers[0].name",
       "a layer with no name"},
      {"{\"layers\": [{\"bit\": 1, \"name\": \"\"}]}", "physics.layers[0].name",
       "an empty name"},
      {"{\"layers\": [{\"bit\": 1, \"name\": \"Two words\"}]}",
       "physics.layers[1].name", "a name that is not a token"},
      {"{\"layers\": [{\"bit\": 1, \"name\": "
       "\"abcdefghijklmnopqrstuvwxyz012345\"}]}",
       "physics.layers[0].name", "a 32-character name, refused not cut"},
      {"{\"layers\": [{\"bit\": 1, \"name\": \"A\"}, {\"bit\": 1, "
       "\"name\": \"B\"}]}",
       "physics.layers[1].bit", "a repeated bit"},
      {"{\"layers\": [{\"bit\": 1, \"name\": \"Water\"}, {\"bit\": 2, "
       "\"name\": \"WATER\"}]}",
       "physics.layers[2].name", "a name repeated ignoring case"},
      {"{\"layers\": [{\"bit\": 1, \"name\": \"A\", \"color\": 1}]}",
       "physics.layers[0].color", "an unknown layer key"},
      {"{\"ignoredPairs\": [[1]]}", "physics.ignoredPairs[0]",
       "a pair of one layer"},
      {"{\"ignoredPairs\": [[1, 2, 3]]}", "physics.ignoredPairs[0]",
       "a pair of three layers"},
      {"{\"ignoredPairs\": [[1, 32]]}", "physics.ignoredPairs[0]",
       "a pair naming a bit past 31"},
      {"{\"ignoredPairs\": [[1, 2], [2, 1]]}", "physics.ignoredPairs[1]",
       "a pair repeated the other way round"},
      {"{\"ignoredPairs\": [\"1,2\"]}", "physics.ignoredPairs[0]",
       "a pair that is not an array"},
  };
  for (const Refusal &refusal : refusals) {
    char label[160] = {};
    std::snprintf(label, sizeof(label), "%s is refused", refusal.what);
    t.check(refused_for(with_physics(refusal.physics), refusal.field), label);
  }

  std::unique_ptr<ct::ProjectDocument> invalid = fresh();
  t.check(ct::parse_project_document(plain.data(), plain.size(), invalid.get())
              .has_value(),
          "the reference parses for the writer checks");
  invalid->collisionLayers.collides[4] &= ~(1U << 6U);
  t.check(!ct::format_project_document(*invalid, out.get(),
                                       ct::kMaxProjectDocumentBytes, &length) &&
              (out[0] == '\0'),
          "the writer refuses a matrix that is not symmetric");
  ct::set_collision_layer_pair(&invalid->collisionLayers, 4U, 6U, false);
  t.check(ct::format_project_document(*invalid, out.get(),
                                      ct::kMaxProjectDocumentBytes, &length),
          "set_collision_layer_pair keeps the matrix symmetric");
  ct::set_collision_layer_pair(&invalid->collisionLayers, 4U, 6U, true);
  t.check(ct::collision_layers_are_default(invalid->collisionLayers),
          "re-allowing the pair restores the default matrix");
  std::snprintf(invalid->collisionLayers.names[7],
                sizeof(invalid->collisionLayers.names[7]), "%s", "a/b");
  t.check(!ct::format_project_document(*invalid, out.get(),
                                       ct::kMaxProjectDocumentBytes, &length),
          "the writer refuses a name its reader would refuse");
}

int main() {
  engine::tests::TestContext t;
  check_round_trip(t);
  check_script_limits(t);
  check_save_settings(t);
  check_packages(t);
  check_collision_layers(t);
  check_refusals(t);
  check_scene_bounds(t);
  check_file_outcomes(t);
  return t.finish("project_document");
}
