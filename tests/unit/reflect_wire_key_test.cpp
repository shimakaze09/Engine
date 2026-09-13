// Verifies that the reflected component codec serializes a field under
// its declared wire key, not its C++ member name (#177): a type whose
// member was renamed behind REFLECT_FIELD_KEY writes the legacy key and
// reads it back, so the rename changes no saved bytes, while an object
// carrying the member name instead of the key is ignored like any other
// unknown field.

#include <cstdint>
#include <cstring>

#include "../test_harness.h"
#include "engine/core/json.h"
#include "engine/core/reflect.h"
#include "serialization_util.h"

namespace {

/// `renamedMember` was once `legacy_key`; the key keeps the wire stable.
struct KeyedComponent final {
  float renamedMember = 0.0F;
  std::uint32_t plain = 0U;
};

} // namespace

REFLECT_TYPE(KeyedComponent)
REFLECT_FIELD_KEY(renamedMember, "legacy_key", Float)
REFLECT_FIELD(plain, Uint32)
REFLECT_END()

/// Runs this executable or test program.
int main() {
  engine::tests::TestContext ctx;
  const engine::core::TypeDescriptor *desc =
      engine::core::global_type_registry().find_type("KeyedComponent");
  ctx.check(desc != nullptr, "keyed type registered");
  if (desc == nullptr) {
    return ctx.finish("reflect_wire_key");
  }
  ctx.check(desc->fieldCount == 2U, "both fields registered");

  KeyedComponent source{};
  source.renamedMember = 2.5F;
  source.plain = 7U;

  engine::core::JsonWriter writer{};
  writer.begin_object();
  ctx.check(engine::runtime::write_reflected_component(writer, "keyed", *desc,
                                                       &source),
            "write through the reflected codec");
  writer.end_object();
  ctx.check(writer.ok(), "writer completed");
  const char *text = writer.result();
  ctx.check((text != nullptr) && (std::strstr(text, "\"legacy_key\"") != nullptr),
            "output carries the wire key");
  ctx.check((text != nullptr) &&
                (std::strstr(text, "renamedMember") == nullptr),
            "output never carries the member name");
  ctx.check((text != nullptr) && (std::strstr(text, "\"plain\"") != nullptr),
            "a field without an explicit key uses its member name");

  engine::core::JsonParser parser{};
  ctx.check((text != nullptr) &&
                parser.parse(text, writer.result_size()),
            "output parses");
  engine::core::JsonValue component{};
  ctx.check((parser.root() != nullptr) &&
                parser.get_object_field(*parser.root(), "keyed", &component),
            "component object present");
  KeyedComponent loaded{};
  ctx.check(engine::runtime::read_reflected_component(parser, component, *desc,
                                                      &loaded),
            "read through the reflected codec");
  ctx.check((loaded.renamedMember == 2.5F) && (loaded.plain == 7U),
            "round trip restores both fields by key");

  // A document written under the member name is a foreign field: the
  // reader leaves the default in place instead of guessing the rename.
  const char kMemberNamed[] = "{\"renamedMember\": 9.0, \"plain\": 3}";
  engine::core::JsonParser memberParser{};
  ctx.check(memberParser.parse(kMemberNamed, sizeof(kMemberNamed) - 1U),
            "member-named document parses");
  KeyedComponent ignored{};
  ctx.check((memberParser.root() != nullptr) &&
                engine::runtime::read_reflected_component(
                    memberParser, *memberParser.root(), *desc, &ignored),
            "member-named document reads");
  ctx.check((ignored.renamedMember == 0.0F) && (ignored.plain == 3U),
            "member name is not a key: the renamed field keeps its default");

  return ctx.finish("reflect_wire_key");
}
