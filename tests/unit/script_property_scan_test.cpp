// Verifies the static scan of a script's property declarations (#134):
// - each literal kind declares its type and default, the table form adds a
//   type, a range and a tooltip, and an integer default fits a float;
// - the declaration is found as a module field and as a field of a
//   top-level table constructor, and comments and strings that mention it
//   are skipped;
// - what the scan cannot read is a diagnostic with its line, never a
//   silent omission: a computed default, a declaration inside a function,
//   a second table, a repeated name, an unknown field, a mismatched or
//   unknown type, an over-long name or string, an unterminated comment;
// - a script declaring nothing yields an empty schema.

#include <cmath>
#include <cstring>
#include <string>

#include "engine/scripting/script_property_scan.h"

#include "../test_harness.h"

namespace {

using engine::math::ScriptPropertyType;
using engine::scripting::find_script_property;
using engine::scripting::scan_script_properties;
using engine::scripting::ScriptPropertyDecl;
using engine::scripting::ScriptPropertySchema;

engine::tests::TestContext g_tests{};

ScriptPropertySchema scan(const std::string &source) {
  ScriptPropertySchema schema{};
  scan_script_properties(source.data(), source.size(), &schema);
  return schema;
}

/// True when some diagnostic is on `line` and its message holds `needle`.
bool has_diagnostic(const ScriptPropertySchema &schema, std::uint32_t line,
                    const char *needle) {
  for (std::size_t i = 0U; i < schema.diagnosticCount; ++i) {
    if ((schema.diagnostics[i].line == line) &&
        (std::strstr(schema.diagnostics[i].message, needle) != nullptr)) {
      return true;
    }
  }
  return false;
}

void check_literal_kinds() {
  const ScriptPropertySchema schema = scan("local M = {}\n"
                                           "M.properties = {\n"
                                           "  speed = 0.25,\n"
                                           "  lives = 3,\n"
                                           "  enabled = true,\n"
                                           "  target = \"Player\",\n"
                                           "  drop = -1.5e1;\n"
                                           "  mask = 0x10,\n"
                                           "}\n"
                                           "return M\n");
  g_tests.check(schema.declared && (schema.count == 6U) &&
                    (schema.diagnosticCount == 0U),
                "six literal properties are read with no diagnostic");
  const ScriptPropertyDecl *speed = find_script_property(schema, "speed");
  g_tests.check((speed != nullptr) &&
                    (speed->defaultValue.type == ScriptPropertyType::Float) &&
                    (speed->defaultValue.floatValue == 0.25F) &&
                    (speed->line == 3U),
                "a number with a '.' is a float, on its own line");
  const ScriptPropertyDecl *lives = find_script_property(schema, "lives");
  g_tests.check((lives != nullptr) &&
                    (lives->defaultValue.type == ScriptPropertyType::Integer) &&
                    (lives->defaultValue.integerValue == 3),
                "a number without '.' or exponent is an integer");
  const ScriptPropertyDecl *enabled = find_script_property(schema, "enabled");
  g_tests.check((enabled != nullptr) &&
                    (enabled->defaultValue.type == ScriptPropertyType::Bool) &&
                    enabled->defaultValue.boolValue,
                "true is a bool");
  const ScriptPropertyDecl *target = find_script_property(schema, "target");
  g_tests.check((target != nullptr) &&
                    (target->defaultValue.type == ScriptPropertyType::String) &&
                    (std::strcmp(target->defaultValue.text, "Player") == 0),
                "a quoted string is a string");
  const ScriptPropertyDecl *drop = find_script_property(schema, "drop");
  g_tests.check((drop != nullptr) &&
                    (drop->defaultValue.type == ScriptPropertyType::Float) &&
                    (drop->defaultValue.floatValue == -15.0F),
                "a negative exponent literal is a float");
  const ScriptPropertyDecl *mask = find_script_property(schema, "mask");
  g_tests.check((mask != nullptr) &&
                    (mask->defaultValue.type == ScriptPropertyType::Integer) &&
                    (mask->defaultValue.integerValue == 16),
                "a hexadecimal literal is an integer");
  g_tests.check((std::strcmp(schema.properties[0].name, "speed") == 0) &&
                    (std::strcmp(schema.properties[5].name, "mask") == 0),
                "properties keep their declaration order");
}

void check_table_form() {
  const ScriptPropertySchema schema =
      scan("M.properties = {\n"
           "  amplitude = { type = \"float\", default = 2, min = 0.0,\n"
           "                max = 10, tooltip = \"How far it travels\" },\n"
           "  name = { type = \"string\" },\n"
           "  count = { default = 4 },\n"
           "}\n");
  g_tests.check((schema.count == 3U) && (schema.diagnosticCount == 0U),
                "the table form is read");
  const ScriptPropertyDecl *amplitude =
      find_script_property(schema, "amplitude");
  g_tests.check(
      (amplitude != nullptr) &&
          (amplitude->defaultValue.type == ScriptPropertyType::Float) &&
          (amplitude->defaultValue.floatValue == 2.0F) && amplitude->hasMin &&
          (amplitude->minValue == 0.0) && amplitude->hasMax &&
          (amplitude->maxValue == 10.0) &&
          (std::strcmp(amplitude->tooltip, "How far it travels") == 0),
      "type, an integer default for a float, range and tooltip are read");
  const ScriptPropertyDecl *name = find_script_property(schema, "name");
  g_tests.check((name != nullptr) &&
                    (name->defaultValue.type == ScriptPropertyType::String) &&
                    (name->defaultValue.text[0] == '\0'),
                "a type with no default defaults to the type's zero");
  const ScriptPropertyDecl *count = find_script_property(schema, "count");
  g_tests.check((count != nullptr) &&
                    (count->defaultValue.type == ScriptPropertyType::Integer),
                "a default with no type takes the default's type");
}

void check_placement() {
  const ScriptPropertySchema constructor =
      scan("-- M.properties = { commented = 1 }\n"
           "local note = \"M.properties = { quoted = 1 }\"\n"
           "--[[ M.properties = { block = 1 } ]]\n"
           "return {\n"
           "  properties = { speed = 1.5, [[ignored]] },\n"
           "  on_tick = function(self, dt) end,\n"
           "}\n");
  g_tests.check(constructor.declared && (constructor.count == 1U) &&
                    (find_script_property(constructor, "speed") != nullptr),
                "a field of a top-level table constructor is read; comments "
                "and strings are skipped");
  g_tests.check(has_diagnostic(constructor, 5U, "name = value"),
                "a positional entry is a diagnostic on its line");

  const ScriptPropertySchema inside = scan("local M = {}\n"
                                           "function M.on_begin_play(self)\n"
                                           "  M.properties = { speed = 1.0 }\n"
                                           "end\n"
                                           "return M\n");
  g_tests.check(!inside.declared && (inside.count == 0U) &&
                    has_diagnostic(inside, 3U, "inside a function"),
                "a declaration inside a function is a diagnostic, not read");

  const ScriptPropertySchema twice = scan("M.properties = { a = 1 }\n"
                                          "M.properties = { b = 2 }\n");
  g_tests.check((twice.count == 1U) &&
                    (find_script_property(twice, "a") != nullptr) &&
                    has_diagnostic(twice, 2U, "second properties table"),
                "a second table is a diagnostic and the first is kept");

  const ScriptPropertySchema none =
      scan("local properties = { not_a_declaration = 1 }\n"
           "local M = {}\nreturn M\n");
  g_tests.check(!none.declared && (none.count == 0U) &&
                    (none.diagnosticCount == 0U),
                "a local named properties declares nothing");
}

void check_diagnostics() {
  const ScriptPropertySchema schema =
      scan("local BASE = 2\n"
           "M.properties = {\n"
           "  computed = BASE * 2,\n"
           "  sum = 1 + 2,\n"
           "  speed = 1.0,\n"
           "  speed = 2.0,\n"
           "  bad = { type = \"vec3\" },\n"
           "  wrong = { type = \"integer\", default = \"x\" },\n"
           "  odd = { type = \"float\", colour = 1 },\n"
           "  "
           "a_name_that_is_much_longer_than_thirty_one_characters_for_sure = "
           "1,\n"
           "  long = \"0123456789012345678901234567890123456789012345678\",\n"
           "  escaped = \"\\65\",\n"
           "  fine = false,\n"
           "}\n");
  g_tests.check((schema.count == 3U) &&
                    (find_script_property(schema, "speed") != nullptr) &&
                    (find_script_property(schema, "odd") != nullptr) &&
                    (find_script_property(schema, "fine") != nullptr),
                "the readable declarations are kept, one with an unknown "
                "metadata field included");
  g_tests.check(has_diagnostic(schema, 3U, "not a literal"),
                "a computed default is a diagnostic");
  g_tests.check(has_diagnostic(schema, 4U, "not a literal"),
                "an expression is a diagnostic");
  g_tests.check(has_diagnostic(schema, 6U, "declared twice"),
                "a repeated name is a diagnostic");
  g_tests.check(has_diagnostic(schema, 7U, "type is one of"),
                "an unknown type is a diagnostic");
  g_tests.check(has_diagnostic(schema, 8U, "is not a integer"),
                "a default of the wrong type is a diagnostic");
  g_tests.check(has_diagnostic(schema, 9U, "unknown field"),
                "an unknown field is a diagnostic");

  const ScriptPropertySchema metadata =
      scan("M.properties = {\n"
           "  speed = { default = 1.0, tooltip = \"a\" .. \"b\", min = x },\n"
           "  lives = { type = \"integer\", default = 2 + 1 },\n"
           "}\n");
  g_tests.check(
      (metadata.count == 1U) &&
          (find_script_property(metadata, "speed") != nullptr) &&
          !find_script_property(metadata, "speed")->hasMin &&
          has_diagnostic(metadata, 2U, "'tooltip' is not a literal") &&
          has_diagnostic(metadata, 2U, "'min' is not a literal") &&
          has_diagnostic(metadata, 3U, "'default' is not a literal"),
      "a bad tooltip or range is reported and the property kept; a "
      "bad default drops it");
  g_tests.check(has_diagnostic(schema, 10U, "longer than 31"),
                "an over-long name is a diagnostic");
  g_tests.check(has_diagnostic(schema, 11U, "longer than 47"),
                "an over-long string is a diagnostic");
  g_tests.check(has_diagnostic(schema, 12U, "escape"),
                "an escape the scan does not read is a diagnostic");

  const ScriptPropertySchema unterminated =
      scan("M.properties = { a = 1 }\n--[[ never closed\n");
  g_tests.check((unterminated.count == 1U) &&
                    has_diagnostic(unterminated, 2U, "unterminated"),
                "an unterminated comment is a diagnostic after what was read");
}

void check_capacity() {
  std::string source = "M.properties = {\n";
  for (int i = 0; i < 17; ++i) {
    source += "  p" + std::to_string(i) + " = " + std::to_string(i) + ",\n";
  }
  source += "}\n";
  const ScriptPropertySchema schema = scan(source);
  g_tests.check((schema.count == ScriptPropertySchema::kMaxProperties) &&
                    has_diagnostic(schema, 18U, "past the 16"),
                "a seventeenth property is a diagnostic, the first sixteen "
                "are kept");

  const ScriptPropertySchema empty = scan("");
  g_tests.check(!empty.declared && (empty.count == 0U),
                "an empty script declares nothing");
}

} // namespace

/// Runs the script property scan suite.
int main() {
  check_literal_kinds();
  check_table_form();
  check_placement();
  check_diagnostics();
  check_capacity();
  return g_tests.finish("script property scan");
}
