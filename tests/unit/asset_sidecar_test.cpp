// Pins the authored sidecar's contract on a scratch tree: the path it
// derives and the predicate recognising one, a write/read round trip for an
// asset and for a folder, labels (the exact bytes with and without them, a
// round trip beside import settings, and a refusal per malformed form),
// each asset type's own import settings (texture and audio: a round trip,
// refusal on another type's asset, a refusal per malformed field), a
// folder's one block per type (the bytes, a round trip, a refusal per
// malformed form), the
// atomic write refusing a nil identity, keys a newer build wrote surviving
// an older build's rewrite (and being named on read), and — the
// part that matters most — each read failure reporting which failure it was, so
// no caller can mistake "could not read the identity" for "there is no identity
// yet" and mint a new one over the top of a live asset.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "../test_harness.h"
#include "engine/content/asset_sidecar.h"
#include "engine/core/logging.h"

namespace {

namespace ct = engine::content;

constexpr const char *kRoot = "asset_sidecar_test_root";

std::string root_path(const char *leaf) {
  return std::string(kRoot) + "/" + leaf;
}

bool write_text(const std::string &path, const char *text) noexcept {
  std::ofstream out(path, std::ios::binary);
  out << text;
  return out.good();
}

bool read_text(const std::string &path, std::string *out) noexcept {
  std::ifstream in(path, std::ios::binary);
  if (!in.good()) {
    return false;
  }
  out->assign((std::istreambuf_iterator<char>(in)),
              std::istreambuf_iterator<char>());
  return true;
}

void test_sidecar_path(engine::tests::TestContext &ctx) noexcept {
  char out[64] = {};
  ctx.check(ct::asset_sidecar_path("assets/props/coin.gltf", out,
                                   sizeof(out)) &&
                (std::strcmp(out, "assets/props/coin.gltf.meta") == 0),
            "the sidecar sits beside the asset, named after the whole file");
  // The whole filename, not the stem: coin.gltf and coin.png in one
  // folder must not share a sidecar.
  char png[64] = {};
  ctx.check(ct::asset_sidecar_path("assets/props/coin.png", png,
                                   sizeof(png)) &&
                (std::strcmp(png, "assets/props/coin.png.meta") == 0) &&
                (std::strcmp(png, out) != 0),
            "two assets sharing a stem get two sidecars");

  char tiny[8] = {};
  ctx.check(!ct::asset_sidecar_path("assets/props/coin.gltf", tiny,
                                    sizeof(tiny)) &&
                (tiny[0] == '\0'),
            "a path that will not fit whole is refused, not truncated");
  ctx.check(!ct::asset_sidecar_path(nullptr, out, sizeof(out)) &&
                !ct::asset_sidecar_path("", out, sizeof(out)) &&
                !ct::asset_sidecar_path("x", nullptr, 8U),
            "null and empty arguments are refused");
}

void test_round_trip(engine::tests::TestContext &ctx) noexcept {
  const std::string asset = root_path("coin.gltf");
  ctx.check(write_text(asset, "not a real glTF"), "the asset file exists");

  ct::AssetSidecar written{};
  written.guid = ct::generate_asset_guid();
  ctx.check(ct::asset_guid_is_valid(written.guid), "a GUID was generated");
  ctx.check(ct::write_asset_sidecar(asset.c_str(), written),
            "the sidecar is written");

  ct::AssetSidecar read{};
  ctx.check((ct::read_asset_sidecar(asset.c_str(), &read) ==
             ct::SidecarReadResult::Ok) &&
                (read.guid == written.guid) && !read.folder &&
                (read.schemaVersion == ct::kAssetSidecarSchemaVersion),
            "the sidecar reads back the identity it was given");

  // The identity survives an edit to the asset's own bytes: that is the
  // whole point of keeping it in a separate file.
  ctx.check(write_text(asset, "edited contents, entirely different"),
            "the asset's contents are edited");
  ct::AssetSidecar afterEdit{};
  ctx.check((ct::read_asset_sidecar(asset.c_str(), &afterEdit) ==
             ct::SidecarReadResult::Ok) &&
                (afterEdit.guid == written.guid),
            "editing the asset does not change its identity");

  // And a rename carries it, because the sidecar moves with the file.
  const std::string renamed = root_path("coin_renamed.gltf");
  std::error_code ec{};
  std::filesystem::rename(asset, renamed, ec);
  std::filesystem::rename(asset + ".meta", renamed + ".meta", ec);
  ct::AssetSidecar afterRename{};
  ctx.check(!ec &&
                (ct::read_asset_sidecar(renamed.c_str(), &afterRename) ==
                 ct::SidecarReadResult::Ok) &&
                (afterRename.guid == written.guid),
            "renaming the asset and its sidecar keeps the identity");
  // Nothing is left behind at the old path to claim the same identity.
  ct::AssetSidecar atOldPath{};
  ctx.check(ct::read_asset_sidecar(asset.c_str(), &atOldPath) ==
                ct::SidecarReadResult::Absent,
            "the old path has no sidecar after the rename");

  // One field per line, so two branches that both imported assets merge
  // per field instead of per file.
  std::string document{};
  ctx.check(read_text(renamed + ".meta", &document), "the document is on disk");
  std::size_t lines = 0U;
  for (const char ch : document) {
    lines += (ch == '\n') ? 1U : 0U;
  }
  ctx.check(lines >= 4U, "the document is written one field per line");
  ctx.check(document.find("\"guid\"") != std::string::npos,
            "the document names the guid field");
  char guidText[ct::kAssetGuidTextLength + 1U] = {};
  ctx.check(ct::format_asset_guid(written.guid, guidText, sizeof(guidText)) &&
                (document.find(guidText) != std::string::npos),
            "the guid is stored as canonical lowercase UUID text");
}

void test_folder_sidecar(engine::tests::TestContext &ctx) noexcept {
  const std::string folder = root_path("props");
  std::error_code ec{};
  std::filesystem::create_directories(folder, ec);

  ct::AssetSidecar written{};
  written.guid = ct::generate_asset_guid();
  written.folder = true;
  ctx.check(!ec && ct::write_asset_sidecar(folder.c_str(), written),
            "a folder's sidecar is written");

  ct::AssetSidecar read{};
  ctx.check((ct::read_asset_sidecar(folder.c_str(), &read) ==
             ct::SidecarReadResult::Ok) &&
                (read.guid == written.guid) && read.folder,
            "a folder reads back as a folder, with its own identity");
}

void test_read_failures_are_distinct(
    engine::tests::TestContext &ctx) noexcept {
  const std::string missing = root_path("never_imported.gltf");
  ctx.check(write_text(missing, "x"), "an asset with no sidecar exists");
  ct::AssetSidecar out{};
  ctx.check(ct::read_asset_sidecar(missing.c_str(), &out) ==
                ct::SidecarReadResult::Absent,
            "an asset that was never imported reads as Absent");

  struct Malformed final {
    const char *leaf;
    const char *document;
    const char *why;
  };
  const Malformed cases[] = {
      {"bad_json.gltf", "{ not json", "invalid JSON is Malformed"},
      {"not_object.gltf", "[1,2,3]", "a non-object root is Malformed"},
      {"no_version.gltf", "{\"guid\":\"01234567-89ab-cdef-fedc-ba9876543210\"}",
       "a missing schemaVersion is Malformed"},
      {"future.gltf",
       "{\"schemaVersion\":9999,"
       "\"guid\":\"01234567-89ab-cdef-fedc-ba9876543210\"}",
       "a schema version from the future is refused, not guessed at"},
      {"no_guid.gltf", "{\"schemaVersion\":1}", "a missing guid is Malformed"},
      {"bad_guid.gltf", "{\"schemaVersion\":1,\"guid\":\"not-a-uuid\"}",
       "a guid that is not canonical UUID text is Malformed"},
      {"nil_guid.gltf",
       "{\"schemaVersion\":1,"
       "\"guid\":\"00000000-0000-0000-0000-000000000000\"}",
       "the nil guid is Malformed: it names no asset"},
      {"bad_folder.gltf",
       "{\"schemaVersion\":1,"
       "\"guid\":\"01234567-89ab-cdef-fedc-ba9876543210\",\"folder\":7}",
       "a non-boolean folder flag is Malformed"},
      {"labels_not_array.gltf",
       "{\"schemaVersion\":1,"
       "\"guid\":\"01234567-89ab-cdef-fedc-ba9876543210\",\"labels\":\"a\"}",
       "labels that are not an array are Malformed"},
      {"label_not_string.gltf",
       "{\"schemaVersion\":1,"
       "\"guid\":\"01234567-89ab-cdef-fedc-ba9876543210\",\"labels\":[3]}",
       "a label that is not a string is Malformed"},
      {"label_empty.gltf",
       "{\"schemaVersion\":1,"
       "\"guid\":\"01234567-89ab-cdef-fedc-ba9876543210\",\"labels\":[\"\"]}",
       "an empty label is Malformed"},
      {"label_space.gltf",
       "{\"schemaVersion\":1,"
       "\"guid\":\"01234567-89ab-cdef-fedc-ba9876543210\","
       "\"labels\":[\"two words\"]}",
       "a label with a space is Malformed"},
      {"label_long.gltf",
       "{\"schemaVersion\":1,"
       "\"guid\":\"01234567-89ab-cdef-fedc-ba9876543210\","
       "\"labels\":[\"abcdefghijklmnopqrstuvwxyz012345\"]}",
       "a 32-character label is refused whole, not shortened"},
      {"label_duplicate.gltf",
       "{\"schemaVersion\":1,"
       "\"guid\":\"01234567-89ab-cdef-fedc-ba9876543210\","
       "\"labels\":[\"Hero\",\"hero\"]}",
       "the same label twice, in any case, is Malformed"},
      {"label_many.gltf",
       "{\"schemaVersion\":1,"
       "\"guid\":\"01234567-89ab-cdef-fedc-ba9876543210\",\"labels\":"
       "[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\",\"j\","
       "\"k\",\"l\",\"m\",\"n\",\"o\",\"p\",\"q\"]}",
       "seventeen labels, one more than an asset carries, are Malformed"},
  };
  for (const Malformed &row : cases) {
    const std::string asset = root_path(row.leaf);
    if (!write_text(asset, "x") || !write_text(asset + ".meta", row.document)) {
      ctx.fail(row.why);
      continue;
    }
    ct::AssetSidecar sidecar{};
    ctx.check(ct::read_asset_sidecar(asset.c_str(), &sidecar) ==
                  ct::SidecarReadResult::Malformed,
              row.why);
  }

  // A sidecar that is too large to be one is Unreadable, not Absent: the
  // bytes on disk may be fine, and a caller must not write over them.
  const std::string huge = root_path("huge.gltf");
  std::string padding(ct::kMaxAssetSidecarBytes + 64U, ' ');
  ctx.check(write_text(huge, "x") && write_text(huge + ".meta",
                                                padding.c_str()),
            "an oversized sidecar is on disk");
  ct::AssetSidecar sidecar{};
  ctx.check(ct::read_asset_sidecar(huge.c_str(), &sidecar) ==
                ct::SidecarReadResult::Unreadable,
            "an oversized sidecar is Unreadable, never Absent");
}

/// A texture's sidecar carries texture settings in the same importSettings
/// block a mesh's carries mesh settings: the block is read as the asset's
/// own type's, a texture with no block reads the defaults and writes none,
/// and a block that will not read, a block on a type with no settings, or a
/// block version from the future is malformed rather than guessed at.
void test_audio_settings(engine::tests::TestContext &ctx) noexcept {
  const std::string sound = root_path("step.wav");
  ct::AssetSidecar sidecar{};
  ctx.check(write_text(sound, "x") &&
                ct::parse_asset_guid("33333333-4444-4555-8666-777777777777",
                                     &sidecar.guid),
            "a sound file and a guid to write");
  sidecar.hasAudioImport = true;
  sidecar.audioImport.sampleRate = 22050U;
  sidecar.audioImport.forceMono = true;
  std::string document{};
  ct::AssetSidecar reread{};
  ctx.check(
      ct::write_asset_sidecar(sound.c_str(), sidecar) &&
          read_text(sound + ".meta", &document) &&
          (document.find("\n    \"sampleRate\": 22050,\n") !=
           std::string::npos) &&
          (document.find("\n    \"forceMono\": true\n") != std::string::npos) &&
          (ct::read_asset_sidecar(sound.c_str(), &reread) ==
           ct::SidecarReadResult::Ok) &&
          reread.hasAudioImport && !reread.hasTextureImport &&
          (reread.audioImport == sidecar.audioImport),
      "audio settings round-trip, one field per line");

  ct::AssetSidecar onTexture = sidecar;
  const std::string texture = root_path("not_a_sound.png");
  ctx.check(write_text(texture, "x") &&
                !ct::write_asset_sidecar(texture.c_str(), onTexture),
            "audio settings on a texture are refused");

  struct Row final {
    const char *block;
    const char *why;
  };
  const Row rows[] = {
      {"{\"sampleRate\": 7999}", "a rate below 8000 Hz"},
      {"{\"sampleRate\": 192001}", "a rate above 192000 Hz"},
      {"{\"sampleRate\": -44100}", "a negative rate"},
      {"{\"sampleRate\": \"44100\"}", "a rate that is not a number"},
      {"{\"forceMono\": 1}", "forceMono that is not a boolean"},
      {"{\"version\": 2}", "an audio block version newer than this build"},
  };
  for (const Row &row : rows) {
    const std::string text = std::string("{\n  \"schemaVersion\": 1,\n  "
                                         "\"guid\": \"33333333-4444-4555-"
                                         "8666-777777777777\",\n  "
                                         "\"importSettings\": ") +
                             row.block + "\n}\n";
    ct::AssetSidecar ignored{};
    ctx.check(write_text(sound + ".meta", text.c_str()) &&
                  (ct::read_asset_sidecar(sound.c_str(), &ignored) ==
                   ct::SidecarReadResult::Malformed),
              row.why);
  }

  // The ends of the range, and 0 for the file's own rate, are accepted.
  const std::uint32_t accepted[] = {0U, ct::kMinAudioImportSampleRate,
                                    ct::kMaxAudioImportSampleRate};
  for (const std::uint32_t rate : accepted) {
    char text[256] = {};
    std::snprintf(text, sizeof(text),
                  "{\n  \"schemaVersion\": 1,\n  \"guid\": "
                  "\"33333333-4444-4555-8666-777777777777\",\n  "
                  "\"importSettings\": {\"sampleRate\": %u}\n}\n",
                  static_cast<unsigned>(rate));
    ct::AssetSidecar read{};
    char message[64] = {};
    std::snprintf(message, sizeof(message), "a rate of %u Hz reads",
                  static_cast<unsigned>(rate));
    ctx.check(write_text(sound + ".meta", text) &&
                  (ct::read_asset_sidecar(sound.c_str(), &read) ==
                   ct::SidecarReadResult::Ok) &&
                  read.hasAudioImport &&
                  (read.audioImport.sampleRate == rate) &&
                  !read.audioImport.forceMono,
              message);
  }
}

void test_folder_settings(engine::tests::TestContext &ctx) noexcept {
  const std::string folder = root_path("art");
  std::error_code ec{};
  std::filesystem::create_directories(folder, ec);
  ct::AssetSidecar sidecar{};
  ctx.check(ct::parse_asset_guid("55555555-6666-4777-8888-999999999999",
                                 &sidecar.guid),
            "a folder guid to write");
  sidecar.folder = true;
  sidecar.hasTextureImport = true;
  sidecar.textureImport.filter = ct::TextureFilterSetting::Nearest;
  sidecar.hasAudioImport = true;
  sidecar.audioImport.forceMono = true;
  std::string document{};
  ct::AssetSidecar reread{};
  ctx.check(ct::write_asset_sidecar(folder.c_str(), sidecar) &&
                read_text(folder + ".meta", &document) &&
                (document.find("\n  \"importSettings\": {\n    "
                               "\"texture\": {\n      \"colorSpace\"") !=
                 std::string::npos) &&
                (document.find("\n    },\n    \"audio\": {\n      "
                               "\"sampleRate\": 0,") != std::string::npos) &&
                (ct::read_asset_sidecar(folder.c_str(), &reread) ==
                 ct::SidecarReadResult::Ok) &&
                reread.folder && !reread.hasMeshImport &&
                reread.hasTextureImport && reread.hasAudioImport &&
                (reread.textureImport == sidecar.textureImport) &&
                (reread.audioImport == sidecar.audioImport),
            "a folder carries one block per type, keyed by the type, one "
            "field per line");

  sidecar.hasMeshImport = true;
  sidecar.meshImport.scaleFactor = 0.01F;
  ctx.check(ct::write_asset_sidecar(folder.c_str(), sidecar) &&
                (ct::read_asset_sidecar(folder.c_str(), &reread) ==
                 ct::SidecarReadResult::Ok) &&
                reread.hasMeshImport &&
                (reread.meshImport.scaleFactor == 0.01F),
            "a folder carries all three blocks together");

  struct Row final {
    const char *block;
    const char *why;
  };
  const Row rows[] = {
      {"{\"textures\": {}}", "a key naming no type"},
      {"{\"texture\": \"nearest\"}", "a type's block that is not an object"},
      {"{\"audio\": {\"sampleRate\": 5}}", "a malformed block inside it"},
      {"{\"mesh\": {\"version\": 9}}", "a block version newer than the "
                                       "build"},
      {"{\"filter\": \"nearest\"}",
       "an asset-shaped block on a folder, which names no type"},
  };
  for (const Row &row : rows) {
    const std::string text = std::string("{\n  \"schemaVersion\": 1,\n  "
                                         "\"guid\": \"55555555-6666-4777-"
                                         "8888-999999999999\",\n  "
                                         "\"folder\": true,\n  "
                                         "\"importSettings\": ") +
                             row.block + "\n}\n";
    ct::AssetSidecar ignored{};
    char message[160] = {};
    std::snprintf(message, sizeof(message), "a folder refuses %s", row.why);
    ctx.check(write_text(folder + ".meta", text.c_str()) &&
                  (ct::read_asset_sidecar(folder.c_str(), &ignored) ==
                   ct::SidecarReadResult::Malformed),
              message);
  }
}

void test_texture_settings(engine::tests::TestContext &ctx) noexcept {
  const std::string texture = root_path("brick.png");
  ctx.check(write_text(texture, "x"), "the texture file exists");
  ct::AssetSidecar sidecar{};
  ct::AssetGuid guid{};
  ctx.check(ct::parse_asset_guid("22222222-3333-4444-8555-666666666666", &guid),
            "a guid to write");
  sidecar.guid = guid;
  ctx.check(ct::write_asset_sidecar(texture.c_str(), sidecar),
            "a texture sidecar with no settings writes");
  std::string document{};
  ct::AssetSidecar read{};
  ctx.check(read_text(texture + ".meta", &document) &&
                (document.find("importSettings") == std::string::npos) &&
                (ct::read_asset_sidecar(texture.c_str(), &read) ==
                 ct::SidecarReadResult::Ok) &&
                !read.hasTextureImport &&
                (read.textureImport == ct::TextureImportSettings{}),
            "no settings write no block and read the defaults");

  sidecar.hasTextureImport = true;
  sidecar.textureImport.colorSpace = ct::TextureColorSpaceSetting::Linear;
  sidecar.textureImport.generateMips = false;
  sidecar.textureImport.filter = ct::TextureFilterSetting::Nearest;
  sidecar.textureImport.wrap = ct::TextureWrapSetting::Clamp;
  ct::AssetSidecar reread{};
  ctx.check(
      ct::write_asset_sidecar(texture.c_str(), sidecar) &&
          read_text(texture + ".meta", &document) &&
          (document.find("\"colorSpace\": \"linear\"") != std::string::npos) &&
          (document.find("\"filter\": \"nearest\"") != std::string::npos) &&
          (ct::read_asset_sidecar(texture.c_str(), &reread) ==
           ct::SidecarReadResult::Ok) &&
          reread.hasTextureImport && !reread.hasMeshImport &&
          (reread.textureImport == sidecar.textureImport),
      "texture settings round-trip, one field per line");

  ct::AssetSidecar meshOnTexture = sidecar;
  meshOnTexture.hasTextureImport = false;
  meshOnTexture.hasMeshImport = true;
  ct::AssetSidecar unchanged{};
  ctx.check(!ct::write_asset_sidecar(texture.c_str(), meshOnTexture) &&
                (ct::read_asset_sidecar(texture.c_str(), &unchanged) ==
                 ct::SidecarReadResult::Ok) &&
                (unchanged.textureImport == sidecar.textureImport),
            "mesh settings on a texture are refused and the file is kept");

  struct Row final {
    const char *leaf;
    const char *block;
    const char *why;
  };
  const Row rows[] = {
      {"bad_space.png", "{\"colorSpace\": \"cmyk\"}",
       "a colour space that is none of auto, srgb or linear"},
      {"bad_filter.png", "{\"filter\": 1}", "a filter that is not a name"},
      {"bad_mips.png", "{\"generateMips\": \"yes\"}",
       "generateMips that is not a boolean"},
      {"future_block.png", "{\"version\": 2}",
       "a texture block version newer than this build reads"},
      {"zero_block.png", "{\"version\": 0}", "a block version of 0"},
      {"settings.lua", "{}", "a block on a type with no import settings"},
  };
  for (const Row &row : rows) {
    const std::string asset = root_path(row.leaf);
    const std::string text = std::string("{\n  \"schemaVersion\": 1,\n  "
                                         "\"guid\": \"22222222-3333-4444-"
                                         "8555-666666666666\",\n  "
                                         "\"importSettings\": ") +
                             row.block + "\n}\n";
    ct::AssetSidecar ignored{};
    ctx.check(write_text(asset, "x") &&
                  write_text(asset + ".meta", text.c_str()) &&
                  (ct::read_asset_sidecar(asset.c_str(), &ignored) ==
                   ct::SidecarReadResult::Malformed),
              row.why);
  }

  const std::string versioned = root_path("versioned.png");
  ct::AssetSidecar explicitVersion{};
  ctx.check(write_text(versioned, "x") &&
                write_text(versioned + ".meta",
                           "{\n  \"schemaVersion\": 1,\n  \"guid\": "
                           "\"22222222-3333-4444-8555-666666666666\",\n  "
                           "\"importSettings\": {\"version\": 1, \"wrap\": "
                           "\"clamp\"}\n}\n") &&
                (ct::read_asset_sidecar(versioned.c_str(), &explicitVersion) ==
                 ct::SidecarReadResult::Ok) &&
                (explicitVersion.textureImport.wrap ==
                 ct::TextureWrapSetting::Clamp) &&
                explicitVersion.textureImport.generateMips,
            "version 1 reads, and an absent field keeps its default");
}

void test_labels(engine::tests::TestContext &ctx) noexcept {
  const std::string asset = root_path("labelled.gltf");
  ctx.check(write_text(asset, "x"), "the asset exists");
  ct::AssetSidecar sidecar{};
  ctx.check(ct::parse_asset_guid("01234567-89ab-cdef-fedc-ba9876543210",
                                 &sidecar.guid),
            "a fixed identity");

  // Without labels the document is exactly what it was before labels
  // existed, so no committed sidecar changes when this ships.
  std::string document{};
  ctx.check(ct::write_asset_sidecar(asset.c_str(), sidecar) &&
                read_text(asset + ".meta", &document) &&
                (document == "{\n  \"schemaVersion\": 1,\n  \"guid\": "
                             "\"01234567-89ab-cdef-fedc-ba9876543210\"\n}\n"),
            "an unlabelled sidecar is byte for byte the two-field document");

  ctx.check(ct::asset_labels_add(&sidecar.labels, "Hero") &&
                ct::asset_labels_add(&sidecar.labels, "env.rock-01") &&
                ct::asset_labels_add(&sidecar.labels, "hero") &&
                (sidecar.labels.count == 2U),
            "labels add once each, whatever their case");
  ctx.check(ct::write_asset_sidecar(asset.c_str(), sidecar) &&
                read_text(asset + ".meta", &document) &&
                (document ==
                 "{\n  \"schemaVersion\": 1,\n  \"guid\": "
                 "\"01234567-89ab-cdef-fedc-ba9876543210\",\n  \"labels\": [\n"
                 "    \"Hero\",\n    \"env.rock-01\"\n  ]\n}\n"),
            "labels are written one per line, in order, after the identity");
  ct::AssetSidecar read{};
  ctx.check((ct::read_asset_sidecar(asset.c_str(), &read) ==
             ct::SidecarReadResult::Ok) &&
                (read.labels == sidecar.labels),
            "the labels read back as written");

  // A label in the author's own script (角色, "character") is written as its
  // UTF-8 bytes and reads back byte for byte (#1185).
  ct::AssetSidecar cjk{};
  cjk.guid = sidecar.guid;
  std::string cjkDocument;
  ct::AssetSidecar cjkRead{};
  ctx.check(ct::asset_labels_add(&cjk.labels, "\xE8\xA7\x92\xE8\x89\xB2") &&
                ct::write_asset_sidecar(asset.c_str(), cjk) &&
                read_text(asset + ".meta", &cjkDocument) &&
                (cjkDocument.find("\"\xE8\xA7\x92\xE8\x89\xB2\"") !=
                 std::string::npos) &&
                (ct::read_asset_sidecar(asset.c_str(), &cjkRead) ==
                 ct::SidecarReadResult::Ok) &&
                (cjkRead.labels == cjk.labels),
            "a CJK label is written as UTF-8 and reads back as written");

  // Labels and import settings live side by side and survive each other.
  sidecar.hasMeshImport = true;
  sidecar.meshImport.scaleFactor = 2.0F;
  ct::AssetSidecar both{};
  ctx.check(ct::write_asset_sidecar(asset.c_str(), sidecar) &&
                (ct::read_asset_sidecar(asset.c_str(), &both) ==
                 ct::SidecarReadResult::Ok) &&
                (both.labels == sidecar.labels) && both.hasMeshImport &&
                (both.meshImport == sidecar.meshImport),
            "labels and import settings round-trip together");

  // Sixteen labels of the longest length fit the document.
  ct::AssetSidecar full{};
  full.guid = sidecar.guid;
  bool added = true;
  for (int i = 0; i < 16; ++i) {
    char label[40] = {};
    std::snprintf(label, sizeof(label), "label_%02d_abcdefghijklmnopqrstuv", i);
    added = added && (std::strlen(label) == 31U) &&
            ct::asset_labels_add(&full.labels, label);
  }
  ct::AssetSidecar fullRead{};
  ctx.check(added && !ct::asset_labels_add(&full.labels, "one_more") &&
                ct::write_asset_sidecar(asset.c_str(), full) &&
                (ct::read_asset_sidecar(asset.c_str(), &fullRead) ==
                 ct::SidecarReadResult::Ok) &&
                (fullRead.labels == full.labels),
            "sixteen 31-character labels fit and round-trip; a seventeenth "
            "is refused");

  ctx.check(!ct::asset_labels_add(&full.labels, "") &&
                !ct::asset_labels_add(&sidecar.labels, "two words") &&
                !ct::asset_labels_add(&sidecar.labels,
                                      "abcdefghijklmnopqrstuvwxyz012345") &&
                (sidecar.labels.count == 2U),
            "an invalid label is refused and the list is unchanged");
  ctx.check(
      ct::asset_labels_remove(&sidecar.labels, "HERO") &&
          (sidecar.labels.count == 1U) &&
          (std::strcmp(sidecar.labels.names[0].data(), "env.rock-01") == 0) &&
          !ct::asset_labels_remove(&sidecar.labels, "hero"),
      "a label is removed by any case, keeping the rest");

  // A label poked into the struct past the checks is refused at write,
  // leaving the file on disk as it was.
  ct::AssetSidecar bad = sidecar;
  bad.labels.names[0].fill('\0');
  std::memcpy(bad.labels.names[0].data(), "no spaces", 9U);
  std::string before{};
  std::string after{};
  ctx.check(read_text(asset + ".meta", &before) &&
                !ct::write_asset_sidecar(asset.c_str(), bad) &&
                read_text(asset + ".meta", &after) && (before == after),
            "a sidecar carrying an invalid label is not written");
}

void test_write_refuses_nil(engine::tests::TestContext &ctx) noexcept {
  const std::string asset = root_path("nil_write.gltf");
  ctx.check(write_text(asset, "x"), "the asset exists");
  ct::AssetSidecar nil{};
  ctx.check(!ct::write_asset_sidecar(asset.c_str(), nil),
            "a sidecar carrying no identity is refused");
  ctx.check(!std::filesystem::exists(asset + ".meta"),
            "the refused write left no file behind");

  // A failed write must never destroy a good identity that is already
  // there: the write is staged and replaced atomically.
  ct::AssetSidecar good{};
  good.guid = ct::generate_asset_guid();
  ctx.check(ct::write_asset_sidecar(asset.c_str(), good),
            "a real identity is written");
  ctx.check(!ct::write_asset_sidecar(asset.c_str(), nil),
            "the nil write is refused again");
  ct::AssetSidecar read{};
  ctx.check((ct::read_asset_sidecar(asset.c_str(), &read) ==
             ct::SidecarReadResult::Ok) &&
                (read.guid == good.guid),
            "the refused write left the previous identity intact");
}

void test_local_ids(engine::tests::TestContext &ctx) noexcept {
  // One source here produces a mesh, a skeleton and three clips, so a
  // reference needs the sub-asset's name as well as the source's GUID.
  ctx.check(ct::asset_local_id("walk.anim") == ct::asset_local_id("walk.anim"),
            "a sub-asset's local id is derived from its name, so it is stable");
  ctx.check(ct::asset_local_id("walk.anim") != ct::asset_local_id("idle.anim"),
            "two sub-assets of one source have different local ids");
  ctx.check(ct::asset_local_id("walk.anim") != ct::asset_local_id("skel"),
            "sub-assets of different kinds differ too");
  ctx.check((ct::asset_local_id(nullptr) == 0U) &&
                (ct::asset_local_id("") == 0U),
            "the primary asset's local id is zero");

  const ct::AssetGuid guid = ct::generate_asset_guid();
  const ct::AssetRef primary = ct::asset_ref_primary(guid);
  const ct::AssetRef clip{guid, ct::asset_local_id("walk.anim")};
  ctx.check(ct::asset_ref_is_valid(primary) && ct::asset_ref_is_valid(clip),
            "both references name an asset");
  ctx.check(!(primary == clip),
            "the source and its sub-asset are different references");
  ctx.check(primary.guid == clip.guid,
            "both still name the same source file");
  ctx.check(!ct::asset_ref_is_valid(ct::AssetRef{}),
            "a default reference names nothing");
}

} // namespace

/// Runs this executable or test program.
void test_sidecar_predicate(engine::tests::TestContext &ctx) noexcept {
  char derived[64] = {};
  ctx.check(ct::asset_sidecar_path("assets/props/coin.png", derived,
                                   sizeof(derived)) &&
                ct::is_asset_sidecar_path(derived),
            "every derived sidecar path is recognised as a sidecar");
  ctx.check(ct::is_asset_sidecar_path("assets/props.meta"),
            "a folder's sidecar is a sidecar");
  ctx.check(!ct::is_asset_sidecar_path("assets/props/coin.png") &&
                !ct::is_asset_sidecar_path("assets/coin.mesh.cookmeta") &&
                !ct::is_asset_sidecar_path("assets/coin.metadata"),
            "an asset, a cook record and a longer suffix are not sidecars");
  ctx.check(!ct::is_asset_sidecar_path(".meta") &&
                !ct::is_asset_sidecar_path("") &&
                !ct::is_asset_sidecar_path(nullptr),
            "the bare suffix, empty and null are not sidecars");
}

std::vector<std::string> g_warnings{};

void note_warning(engine::core::LogLevel level, const char * /*channel*/,
                  const char *message, void * /*userData*/) noexcept {
  if ((level == engine::core::LogLevel::Warning) && (message != nullptr)) {
    g_warnings.emplace_back(message);
  }
}

std::size_t count_of(const std::string &text, const char *needle) {
  std::size_t count = 0U;
  for (std::size_t at = text.find(needle); at != std::string::npos;
       at = text.find(needle, at + 1U)) {
    ++count;
  }
  return count;
}

/// A sidecar a newer build wrote keeps its new keys through an older
/// build's rewrite (here a label edit, the editor's path): every top-level
/// member the reader does not know is carried verbatim, once, while a
/// known key the writer now omits (all labels removed) stays gone. The
/// read names each unknown key, nested ones included.
void test_unknown_keys_survive_a_rewrite(
    engine::tests::TestContext &ctx) noexcept {
  const std::string asset = root_path("newer.gltf");
  ctx.check(write_text(asset, "x") &&
                write_text(asset + ".meta",
                           "{\n  \"schemaVersion\": 1,\n  \"guid\": "
                           "\"11111111-2222-4333-8444-555555555555\",\n"
                           "  \"colorSpace\": \"linear\",\n"
                           "  \"importSettings\": {\"meshIndex\": 0, "
                           "\"primitiveIndex\": -1, \"scaleFactor\": 1, "
                           "\"upAxis\": 1, \"generateNormals\": false, "
                           "\"futureNested\": 3},\n"
                           "  \"labels\": [\"old\"],\n"
                           "  \"futureBlock\": {\"a\": [1, 2]}\n}\n"),
            "a sidecar with keys from a newer build");
  g_warnings.clear();
  ct::AssetSidecar sidecar{};
  ctx.check(ct::read_asset_sidecar(asset.c_str(), &sidecar) ==
                ct::SidecarReadResult::Ok,
            "it still reads");
  std::size_t named = 0U;
  for (const std::string &warning : g_warnings) {
    if ((warning.find("'colorSpace'") != std::string::npos) ||
        (warning.find("'futureBlock'") != std::string::npos) ||
        (warning.find("'importSettings.futureNested'") != std::string::npos)) {
      ++named;
    }
  }
  ctx.check((named == 3U) && (g_warnings.size() == 3U),
            "the read names each unknown key, nested ones included");

  sidecar.labels = ct::AssetLabels{};
  ctx.check(ct::asset_labels_add(&sidecar.labels, "relabelled"),
            "relabel as the editor does");
  std::string document{};
  ctx.check(ct::write_asset_sidecar(asset.c_str(), sidecar) &&
                read_text(asset + ".meta", &document),
            "the rewrite succeeds");
  ctx.check((count_of(document, "\"colorSpace\": \"linear\"") == 1U) &&
                (count_of(document, "\"futureBlock\": {\"a\": [1, 2]}") == 1U),
            "each top-level key the build does not know is carried once, "
            "verbatim");
  ctx.check((document.find("\"relabelled\"") != std::string::npos) &&
                (document.find("\"old\"") == std::string::npos),
            "the known keys carry the new values");

  sidecar.labels = ct::AssetLabels{};
  ct::AssetSidecar reread{};
  ctx.check(ct::write_asset_sidecar(asset.c_str(), sidecar) &&
                read_text(asset + ".meta", &document) &&
                (document.find("\"labels\"") == std::string::npos) &&
                (count_of(document, "\"colorSpace\"") == 1U) &&
                (ct::read_asset_sidecar(asset.c_str(), &reread) ==
                 ct::SidecarReadResult::Ok),
            "a known key the writer omits is not carried back, and the "
            "result still reads");
}

int main() {
  engine::tests::TestContext ctx;
  ctx.check(engine::core::initialize_logging(), "initialize logging");

  std::error_code ec{};
  std::filesystem::remove_all(kRoot, ec);
  std::filesystem::create_directories(kRoot, ec);
  if (ec) {
    ctx.fail("the scratch tree could be created");
    return ctx.finish("asset sidecar");
  }

  test_sidecar_path(ctx);
  test_sidecar_predicate(ctx);
  test_round_trip(ctx);
  test_folder_sidecar(ctx);
  test_read_failures_are_distinct(ctx);
  test_labels(ctx);
  test_texture_settings(ctx);
  test_audio_settings(ctx);
  test_folder_settings(ctx);
  ctx.check(engine::core::log_register_sink(&note_warning, nullptr),
            "warning sink");
  test_unknown_keys_survive_a_rewrite(ctx);
  engine::core::log_unregister_sink(&note_warning, nullptr);
  test_write_refuses_nil(ctx);
  test_local_ids(ctx);

  std::filesystem::remove_all(kRoot, ec);
  engine::core::shutdown_logging();
  return ctx.finish("asset sidecar");
}
