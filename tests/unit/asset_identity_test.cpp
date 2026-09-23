// Pins the three asset identities' contracts: v4 GUID generation and its
// canonical text round trip with strict parsing, the built-in GUID's
// derivation from the canonical path, the reference's two text shapes and
// their strict parse, the canonical-path key's spelling independence and
// case sensitivity, the case-only portability conflict check, and the
// content hash. Also pins that the three are distinct types the compiler
// will not let a caller interchange.

#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>

#include "../test_harness.h"
#include "engine/content/asset_identity.h"

namespace {

namespace ct = engine::content;

// The three identities answer different questions, so none of them may be
// assignable from or convertible to another. If any of these ever becomes
// false, a call site can silently store a content hash where a persistent
// identity belongs.
static_assert(!std::is_convertible_v<ct::AssetGuid, ct::PathKey>,
              "a GUID must not convert to a path key");
static_assert(!std::is_convertible_v<ct::PathKey, ct::ContentHash>,
              "a path key must not convert to a content hash");
static_assert(!std::is_convertible_v<ct::ContentHash, ct::PathKey>,
              "a content hash must not convert to a path key");
static_assert(!std::is_convertible_v<ct::PathKey, std::uint64_t>,
              "a path key must not decay to its underlying integer");
static_assert(!std::is_convertible_v<std::uint64_t, ct::PathKey>,
              "an integer must not become a path key implicitly");

void test_guid_generation(engine::tests::TestContext &ctx) noexcept {
  const ct::AssetGuid first = ct::generate_asset_guid();
  const ct::AssetGuid second = ct::generate_asset_guid();

  ctx.check(ct::asset_guid_is_valid(first) && ct::asset_guid_is_valid(second),
            "a generated GUID is valid");
  ctx.check(!(first == second), "two generated GUIDs differ");
  ctx.check(!ct::asset_guid_is_valid(ct::kNilAssetGuid),
            "the nil GUID is not valid");

  // UUID v4: version 4 in the high nibble of byte 6, variant 10xx in the
  // top two bits of byte 8. Byte 6 is bits 8..15 of the high half's low
  // 32 bits; byte 8 is the top byte of the low half.
  for (const ct::AssetGuid &guid : {first, second}) {
    const unsigned int versionNibble =
        static_cast<unsigned int>((guid.high >> 12U) & 0xFULL);
    const unsigned int variantBits =
        static_cast<unsigned int>((guid.low >> 62U) & 0x3ULL);
    ctx.check(versionNibble == 4U, "the version nibble says v4");
    ctx.check(variantBits == 2U, "the variant bits are 10xx");
  }
}

void test_guid_text_round_trip(engine::tests::TestContext &ctx) noexcept {
  const ct::AssetGuid guid{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};
  char text[ct::kAssetGuidTextLength + 1U] = {};
  ctx.check(ct::format_asset_guid(guid, text, sizeof(text)),
            "a GUID formats into an exactly sized buffer");
  ctx.check(std::strlen(text) == ct::kAssetGuidTextLength,
            "the text form is 36 characters");
  ctx.check(std::strcmp(text, "01234567-89ab-cdef-fedc-ba9876543210") == 0,
            "the text form is canonical lowercase 8-4-4-4-12");

  ct::AssetGuid parsed{};
  ctx.check(ct::parse_asset_guid(text, &parsed) && (parsed == guid),
            "the canonical text parses back to the same GUID");

  // Read leniently on case, write canonically: an author who upper-cases
  // a sidecar by hand has not broken their project.
  ct::AssetGuid upper{};
  ctx.check(ct::parse_asset_guid("01234567-89AB-CDEF-FEDC-BA9876543210",
                                 &upper) &&
                (upper == guid),
            "uppercase text parses to the same GUID");

  // A generated GUID survives the round trip too, so nothing in the
  // version or variant bits confuses the formatter.
  const ct::AssetGuid generated = ct::generate_asset_guid();
  char generatedText[ct::kAssetGuidTextLength + 1U] = {};
  ct::AssetGuid reparsed{};
  ctx.check(ct::format_asset_guid(generated, generatedText,
                                  sizeof(generatedText)) &&
                ct::parse_asset_guid(generatedText, &reparsed) &&
                (reparsed == generated),
            "a generated GUID round trips through its text form");

  char tooSmall[ct::kAssetGuidTextLength] = {};
  ctx.check(!ct::format_asset_guid(guid, tooSmall, sizeof(tooSmall)) &&
                (tooSmall[0] == '\0'),
            "formatting refuses a buffer one byte short and empties it");
  ctx.check(!ct::format_asset_guid(guid, nullptr, 64U),
            "formatting refuses a null destination");
}

void test_guid_parse_is_strict(engine::tests::TestContext &ctx) noexcept {
  const char *refused[] = {
      nullptr,
      "",
      "0123456789abcdeffedcba9876543210",       // no hyphens
      "01234567-89ab-cdef-fedc-ba98765432",     // too short
      "01234567-89ab-cdef-fedc-ba98765432100",  // too long
      "01234567-89ab-cdef-fedc_ba9876543210",   // wrong separator
      "0123456789ab-cdef-fedc-ba9876543210-0",  // hyphens misplaced
      "0123456g-89ab-cdef-fedc-ba9876543210",   // non-hex digit
      " 1234567-89ab-cdef-fedc-ba9876543210",   // leading space
      "{01234567-89ab-cdef-fedc-ba9876543210}", // braced form
  };
  for (const char *text : refused) {
    ct::AssetGuid parsed{0xAAAAAAAAAAAAAAAAULL, 0xBBBBBBBBBBBBBBBBULL};
    const bool ok = ct::parse_asset_guid(text, &parsed);
    if (ok || !(parsed == ct::kNilAssetGuid)) {
      std::printf("  parse accepted or left a value for '%s'\n",
                  (text != nullptr) ? text : "(null)");
    }
    ctx.check(!ok && (parsed == ct::kNilAssetGuid),
              "a malformed GUID is refused and leaves the output nil");
  }
  ctx.check(!ct::parse_asset_guid("01234567-89ab-cdef-fedc-ba9876543210",
                                  nullptr),
            "parsing refuses a null destination");

  // The nil GUID has a text form and parses, but is still not valid: the
  // two questions are separate.
  ct::AssetGuid nil{0x1ULL, 0x2ULL};
  ctx.check(ct::parse_asset_guid("00000000-0000-0000-0000-000000000000",
                                 &nil) &&
                (nil == ct::kNilAssetGuid) && !ct::asset_guid_is_valid(nil),
            "the nil text form parses to a nil, invalid GUID");
}

void test_guid_hash_and_order(engine::tests::TestContext &ctx) noexcept {
  const ct::AssetGuid a{1ULL, 2ULL};
  const ct::AssetGuid b{1ULL, 3ULL};
  const ct::AssetGuid c{2ULL, 0ULL};

  ctx.check(ct::asset_guid_hash(a) == ct::asset_guid_hash(ct::AssetGuid{1ULL,
                                                                        2ULL}),
            "the hash is a function of the value");
  ctx.check(ct::asset_guid_hash(a) != ct::asset_guid_hash(b),
            "GUIDs differing in the low half hash differently");
  ctx.check(ct::asset_guid_hash(a) != ct::asset_guid_hash(c),
            "GUIDs differing in the high half hash differently");

  ctx.check(ct::asset_guid_precedes(a, b) && !ct::asset_guid_precedes(b, a),
            "the order breaks ties on the low half");
  ctx.check(ct::asset_guid_precedes(b, c) && !ct::asset_guid_precedes(c, b),
            "the high half orders first");
  ctx.check(!ct::asset_guid_precedes(a, a),
            "the order is strict: nothing precedes itself");
}

void test_path_key(engine::tests::TestContext &ctx) noexcept {
  const ct::PathKey canonical = ct::make_path_key("assets/props/coin.mesh");
  ctx.check(ct::path_key_is_valid(canonical), "a real path has a key");

  // The key is derived from the canonical spelling, so every spelling of
  // one location gives one key.
  const char *sameLocation[] = {
      "assets//props/coin.mesh",  "assets\\props\\coin.mesh",
      "./assets/props/coin.mesh", "assets/./props/coin.mesh",
      "assets/props/coin.mesh/",
  };
  for (const char *spelling : sameLocation) {
    ctx.check(ct::make_path_key(spelling) == canonical,
              "every spelling of one location gives one key");
  }

  // Case is preserved on every platform: these are different keys, and
  // the conflict is reported rather than merged.
  ctx.check(!(ct::make_path_key("assets/Foo.png") ==
              ct::make_path_key("assets/foo.png")),
            "paths differing only by case are different keys");
  ctx.check(ct::path_keys_collide_by_case("assets/Foo.png", "assets/foo.png"),
            "a case-only difference is reported as a portability conflict");
  ctx.check(ct::path_keys_collide_by_case("assets//FOO.png", "./assets/foo.png"),
            "the conflict check compares canonical forms");
  ctx.check(!ct::path_keys_collide_by_case("assets/foo.png",
                                           "assets/foo.png"),
            "one path does not conflict with itself");
  ctx.check(!ct::path_keys_collide_by_case("assets/foo.png",
                                           "assets/bar.png"),
            "genuinely different paths are not a case conflict");
  ctx.check(!ct::path_keys_collide_by_case("assets/foo.png",
                                           "assets/foo.pngx"),
            "a prefix is not a case conflict");
  ctx.check(!ct::path_keys_collide_by_case(nullptr, "assets/foo.png"),
            "a path with no canonical form is not a conflict");

  // Paths that name no asset have no key.
  const char *noKey[] = {nullptr, "",  "/",          "//",
                         ".",     "./", "assets/../x", ".."};
  for (const char *path : noKey) {
    const ct::PathKey key = ct::make_path_key(path);
    if (ct::path_key_is_valid(key)) {
      std::printf("  a key was derived for '%s'\n",
                  (path != nullptr) ? path : "(null)");
    }
    ctx.check(!ct::path_key_is_valid(key) && (key == ct::kInvalidPathKey),
              "a path that names no asset has no key");
  }

  ctx.check(!(ct::make_path_key("assets/props/gem.mesh") == canonical),
            "two locations have two keys");
}

void test_content_hash(engine::tests::TestContext &ctx) noexcept {
  const char bytes[] = "the asset's contents";
  const ct::ContentHash hash = ct::make_content_hash(bytes, sizeof(bytes));
  ctx.check(ct::content_hash_is_valid(hash), "real content has a hash");
  ctx.check(ct::make_content_hash(bytes, sizeof(bytes)) == hash,
            "the same bytes hash the same");

  const char edited[] = "the asset's contentt";
  ctx.check(!(ct::make_content_hash(edited, sizeof(edited)) == hash),
            "a one-byte edit changes the hash");
  ctx.check(!(ct::make_content_hash(bytes, sizeof(bytes) - 1U) == hash),
            "a shorter read of the same bytes hashes differently");

  ctx.check(!ct::content_hash_is_valid(ct::make_content_hash(nullptr, 8U)),
            "null content has no hash");
  ctx.check(ct::content_hash_is_valid(ct::make_content_hash("", 0U)),
            "empty content still has a hash a cook can compare");

  // Both are 64-bit FNV-1a, so feeding one the other's bytes produces the
  // same number. That is precisely why the two are kept apart by type and
  // not by value: nothing about the number says which question it
  // answers, and the static_asserts above are what stop a caller storing
  // one where the other belongs.
  const ct::PathKey key = ct::make_path_key("assets/props/coin.mesh");
  const ct::ContentHash ofThePathText =
      ct::make_content_hash("assets/props/coin.mesh", 22U);
  ctx.check(key.value == ofThePathText.value,
            "the numbers can coincide, so only the types keep them apart");
}

void test_builtin_guid(engine::tests::TestContext &ctx) noexcept {
  const ct::AssetGuid cube = ct::builtin_asset_guid("builtin://cube");
  ctx.check(ct::asset_guid_is_valid(cube), "a built-in has a GUID");
  ctx.check(ct::builtin_asset_guid("builtin://cube") == cube,
            "the derivation is deterministic");
  ctx.check(ct::builtin_asset_guid("builtin://./cube") == cube,
            "every spelling of one path derives one GUID");
  ctx.check(!(ct::builtin_asset_guid("builtin://sphere") == cube),
            "two built-ins derive two GUIDs");
  ctx.check(!(ct::builtin_asset_guid("builtin://Cube") == cube),
            "case is identity for a built-in as for any path");

  // Version 8 and the RFC variant, in the same positions v4 uses, so a
  // derived GUID can never coincide with a generated one.
  ctx.check(((cube.high >> 12U) & 0xFULL) == 8U,
            "the version nibble says custom (8), not v4");
  ctx.check(((cube.low >> 62U) & 0x3ULL) == 2U,
            "the variant bits are 10xx");
  ctx.check(!(cube == ct::generate_asset_guid()),
            "a derived GUID is never a generated one");

  ctx.check(!ct::asset_guid_is_valid(ct::builtin_asset_guid(nullptr)),
            "a null path derives nil");
  ctx.check(!ct::asset_guid_is_valid(ct::builtin_asset_guid("")),
            "an empty path derives nil");
  ctx.check(!ct::asset_guid_is_valid(ct::builtin_asset_guid("builtin://../x")),
            "an uncanonicalizable path derives nil");
}

void test_local_id_parse(engine::tests::TestContext &ctx) noexcept {
  std::uint64_t value = 0U;
  ctx.check(ct::parse_asset_local_id("0123456789abcdef", 16U, &value) &&
                (value == 0x0123456789ABCDEFULL),
            "sixteen lowercase hex digits parse");
  ctx.check(!ct::parse_asset_local_id("0123456789abcde", 15U, &value),
            "fifteen digits are refused");
  ctx.check(!ct::parse_asset_local_id("0123456789ABCDEF", 16U, &value),
            "uppercase digits are refused");
  ctx.check(!ct::parse_asset_local_id("0123456789abcdeg", 16U, &value),
            "a non-hex character is refused");
  ctx.check(!ct::parse_asset_local_id(nullptr, 16U, &value),
            "null text is refused");
  ctx.check(!ct::parse_asset_local_id("0123456789abcdef", 16U, nullptr),
            "a null out is refused");
}

void test_ref_text_round_trip(engine::tests::TestContext &ctx) noexcept {
  const ct::AssetGuid guid{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};
  const char *const guidText = "01234567-89ab-cdef-fedc-ba9876543210";
  char text[ct::kAssetRefTextLength + 1U] = {};

  // A primary asset is written as the bare GUID.
  const ct::AssetRef primary = ct::asset_ref_primary(guid);
  ctx.check(ct::format_asset_ref(primary, text, sizeof(text)) &&
                (std::strcmp(text, guidText) == 0),
            "a primary reference formats as the bare GUID");
  ct::AssetRef parsed{};
  ctx.check(ct::parse_asset_ref(text, &parsed) && (parsed == primary),
            "the bare GUID parses back to the primary reference");

  // A sub-asset carries its local id after '#'.
  const ct::AssetRef sub{guid, 0x00FF00FF00FF00FFULL};
  ctx.check(ct::format_asset_ref(sub, text, sizeof(text)) &&
                (std::strcmp(text, "01234567-89ab-cdef-fedc-ba9876543210"
                                   "#00ff00ff00ff00ff") == 0),
            "a sub-asset reference formats as GUID#localId");
  ctx.check(std::strlen(text) == ct::kAssetRefTextLength,
            "the sub-asset form is exactly kAssetRefTextLength long");
  ctx.check(ct::parse_asset_ref(text, &parsed) && (parsed == sub),
            "the sub-asset form parses back whole");

  // Capacity is checked for the shape actually written.
  char tight[ct::kAssetGuidTextLength + 1U] = {};
  ctx.check(ct::format_asset_ref(primary, tight, sizeof(tight)),
            "a primary reference fits a GUID-sized buffer");
  ctx.check(!ct::format_asset_ref(sub, tight, sizeof(tight)) &&
                (tight[0] == '\0'),
            "a sub-asset reference refuses a GUID-sized buffer and empties it");
  char short36[ct::kAssetGuidTextLength] = {};
  ctx.check(!ct::format_asset_ref(primary, short36, sizeof(short36)),
            "a buffer one short of the GUID form is refused");
}

void test_ref_parse_is_strict(engine::tests::TestContext &ctx) noexcept {
  const char *const guidText = "01234567-89ab-cdef-fedc-ba9876543210";
  ct::AssetRef parsed{ct::AssetGuid{1U, 1U}, 7U};

  ctx.check(!ct::parse_asset_ref(nullptr, &parsed) &&
                !ct::asset_ref_is_valid(parsed),
            "null text is refused and the out is nil");
  ctx.check(!ct::parse_asset_ref("", &parsed), "empty text is refused");
  ctx.check(!ct::parse_asset_ref(guidText, nullptr), "a null out is refused");

  std::string text = std::string(guidText) + "#0123456789abcde";
  ctx.check(!ct::parse_asset_ref(text.c_str(), &parsed),
            "a fifteen-digit local id is refused");
  text = std::string(guidText) + "#0123456789ABCDEF";
  ctx.check(!ct::parse_asset_ref(text.c_str(), &parsed),
            "an uppercase local id is refused");
  text = std::string(guidText) + "#0123456789abcdef0";
  ctx.check(!ct::parse_asset_ref(text.c_str(), &parsed),
            "a trailing character is refused");
  text = std::string(guidText) + "%0123456789abcdef";
  ctx.check(!ct::parse_asset_ref(text.c_str(), &parsed),
            "a separator other than '#' is refused");
  text = std::string(guidText) + "#0000000000000000";
  ctx.check(!ct::parse_asset_ref(text.c_str(), &parsed),
            "the sub-asset shape naming the primary is refused");
  text = std::string("01234567-89ab-cdef-fedc-ba987654321") + "#0123456789abcdef";
  ctx.check(!ct::parse_asset_ref(text.c_str(), &parsed),
            "a malformed GUID in the sub-asset shape is refused");
  ctx.check(!ct::asset_ref_is_valid(parsed),
            "every refusal leaves the out nil");
}

} // namespace

/// Runs this executable or test program.
int main() {
  engine::tests::TestContext ctx;
  test_guid_generation(ctx);
  test_guid_text_round_trip(ctx);
  test_guid_parse_is_strict(ctx);
  test_guid_hash_and_order(ctx);
  test_path_key(ctx);
  test_content_hash(ctx);
  test_builtin_guid(ctx);
  test_local_id_parse(ctx);
  test_ref_text_round_trip(ctx);
  test_ref_parse_is_strict(ctx);
  return ctx.finish("asset identity");
}
