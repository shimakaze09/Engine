// Pins the authored sidecar's contract on a scratch tree: the path it
// derives, a write/read round trip for an asset and for a folder, the
// atomic write refusing a nil identity, and — the part that matters most
// — each read failure reporting which failure it was, so no caller can
// mistake "could not read the identity" for "there is no identity yet"
// and mint a new one over the top of a live asset.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

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
      {"no_version.gltf",
       "{\"guid\":\"01234567-89ab-cdef-fedc-ba9876543210\"}",
       "a missing schemaVersion is Malformed"},
      {"future.gltf",
       "{\"schemaVersion\":9999,"
       "\"guid\":\"01234567-89ab-cdef-fedc-ba9876543210\"}",
       "a schema version from the future is refused, not guessed at"},
      {"no_guid.gltf", "{\"schemaVersion\":1}",
       "a missing guid is Malformed"},
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
  test_round_trip(ctx);
  test_folder_sidecar(ctx);
  test_read_failures_are_distinct(ctx);
  test_write_refuses_nil(ctx);
  test_local_ids(ctx);

  std::filesystem::remove_all(kRoot, ec);
  engine::core::shutdown_logging();
  return ctx.finish("asset sidecar");
}
