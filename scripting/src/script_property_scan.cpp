// Implements the static scan of a script's property declarations: a Lua
// lexer just thorough enough to skip comments and strings and to count
// block depth, and a reader for the one literal properties table at the
// script's top level.

#include "engine/scripting/script_property_scan.h"

#include "engine/core/text_parse.h"

#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace engine::scripting {
namespace {

using math::ScriptPropertyType;
using math::ScriptPropertyValue;

enum class TokenKind : std::uint8_t {
  End,
  Name,
  Number,
  String,
  Symbol,
  /// Text the lexer cannot read (an unterminated string or comment).
  Error,
};

struct Token final {
  TokenKind kind = TokenKind::End;
  const char *begin = nullptr;
  std::size_t length = 0U;
  std::uint32_t line = 1U;
  /// For a String token, its value with escapes resolved; empty when it
  /// does not fit or holds an escape the scan does not read.
  char text[ScriptPropertyValue::kMaxTextLength + 1U] = {};
  bool textFits = true;
  bool textReadable = true;
};

class Lexer final {
public:
  Lexer(const char *source, std::size_t length) noexcept
      : m_source(source), m_end(source + length) {}

  Token next() noexcept {
    skip_space_and_comments();
    Token token{};
    token.line = m_line;
    token.begin = m_cursor;
    if (m_error) {
      token.kind = TokenKind::Error;
      return token;
    }
    if (m_cursor >= m_end) {
      return token;
    }
    const char c = *m_cursor;
    if (is_name_start(c)) {
      while ((m_cursor < m_end) && is_name_char(*m_cursor)) {
        ++m_cursor;
      }
      token.kind = TokenKind::Name;
    } else if (is_digit(c) || ((c == '.') && (m_cursor + 1 < m_end) &&
                               is_digit(m_cursor[1]))) {
      read_number();
      token.kind = TokenKind::Number;
    } else if ((c == '"') || (c == '\'')) {
      token.kind =
          read_quoted_string(&token) ? TokenKind::String : TokenKind::Error;
    } else if ((c == '[') && (long_bracket_level(m_cursor) >= 0)) {
      token.kind =
          read_long_string(&token) ? TokenKind::String : TokenKind::Error;
    } else {
      read_symbol();
      token.kind = TokenKind::Symbol;
    }
    token.length = static_cast<std::size_t>(m_cursor - token.begin);
    return token;
  }

private:
  static bool is_digit(char c) noexcept { return (c >= '0') && (c <= '9'); }
  static bool is_name_start(char c) noexcept {
    return ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) ||
           (c == '_');
  }
  static bool is_name_char(char c) noexcept {
    return is_name_start(c) || is_digit(c);
  }

  /// The level of a long bracket opening at `at` ("[[" is 0, "[=[" 1), or
  /// -1 when `at` does not open one.
  int long_bracket_level(const char *at) const noexcept {
    if ((at >= m_end) || (*at != '[')) {
      return -1;
    }
    const char *p = at + 1;
    int level = 0;
    while ((p < m_end) && (*p == '=')) {
      ++level;
      ++p;
    }
    return ((p < m_end) && (*p == '[')) ? level : -1;
  }

  /// Advances past a long bracket body of `level` opened at m_cursor,
  /// copying it into `token` when one is given. False when unterminated.
  bool skip_long_bracket(int level, Token *token) noexcept {
    m_cursor += level + 2;
    // A newline right after the opening bracket is not part of the text.
    if ((m_cursor < m_end) && (*m_cursor == '\r')) {
      ++m_cursor;
    }
    if ((m_cursor < m_end) && (*m_cursor == '\n')) {
      ++m_line;
      ++m_cursor;
    }
    std::size_t written = 0U;
    while (m_cursor < m_end) {
      if (*m_cursor == ']') {
        const char *p = m_cursor + 1;
        int closing = 0;
        while ((p < m_end) && (*p == '=')) {
          ++closing;
          ++p;
        }
        if ((closing == level) && (p < m_end) && (*p == ']')) {
          m_cursor = p + 1;
          return true;
        }
      }
      if (*m_cursor == '\n') {
        ++m_line;
      }
      if (token != nullptr) {
        append(token, &written, *m_cursor);
      }
      ++m_cursor;
    }
    return false;
  }

  void skip_space_and_comments() noexcept {
    while (m_cursor < m_end) {
      const char c = *m_cursor;
      if (c == '\n') {
        ++m_line;
        ++m_cursor;
      } else if ((c == ' ') || (c == '\t') || (c == '\r') || (c == '\f') ||
                 (c == '\v')) {
        ++m_cursor;
      } else if ((c == '-') && (m_cursor + 1 < m_end) && (m_cursor[1] == '-')) {
        const std::uint32_t commentLine = m_line;
        m_cursor += 2;
        const int level = long_bracket_level(m_cursor);
        if (level >= 0) {
          if (!skip_long_bracket(level, nullptr)) {
            m_error = true;
            m_line = commentLine;
            return;
          }
        } else {
          while ((m_cursor < m_end) && (*m_cursor != '\n')) {
            ++m_cursor;
          }
        }
      } else if ((c == '#') && (m_cursor == m_source) &&
                 (m_cursor + 1 < m_end) && (m_cursor[1] == '!')) {
        // A shebang line, which Lua skips too.
        while ((m_cursor < m_end) && (*m_cursor != '\n')) {
          ++m_cursor;
        }
      } else {
        return;
      }
    }
  }

  void read_number() noexcept {
    const bool hex = (m_cursor + 1 < m_end) && (m_cursor[0] == '0') &&
                     ((m_cursor[1] == 'x') || (m_cursor[1] == 'X'));
    if (hex) {
      m_cursor += 2;
    }
    while (m_cursor < m_end) {
      const char c = *m_cursor;
      const bool exponent =
          hex ? ((c == 'p') || (c == 'P')) : ((c == 'e') || (c == 'E'));
      if (exponent && (m_cursor + 1 < m_end) &&
          ((m_cursor[1] == '+') || (m_cursor[1] == '-'))) {
        m_cursor += 2;
      } else if (is_name_char(c) || (c == '.')) {
        ++m_cursor;
      } else {
        return;
      }
    }
  }

  static void append(Token *token, std::size_t *written, char c) noexcept {
    if (*written + 1U >= sizeof(token->text)) {
      token->textFits = false;
      return;
    }
    token->text[(*written)++] = c;
    token->text[*written] = '\0';
  }

  bool read_quoted_string(Token *token) noexcept {
    const char quote = *m_cursor++;
    std::size_t written = 0U;
    while (m_cursor < m_end) {
      char c = *m_cursor++;
      if (c == quote) {
        return true;
      }
      if (c == '\n') {
        return false;
      }
      if (c == '\\') {
        if (m_cursor >= m_end) {
          return false;
        }
        const char escaped = *m_cursor++;
        switch (escaped) {
        case 'n':
          c = '\n';
          break;
        case 't':
          c = '\t';
          break;
        case 'r':
          c = '\r';
          break;
        case '\\':
        case '"':
        case '\'':
          c = escaped;
          break;
        case '\n':
          ++m_line;
          c = '\n';
          break;
        default:
          // \a \b \f \v \z \x \u{} and decimal escapes: skipped over,
          // and the value is reported as unreadable.
          token->textReadable = false;
          continue;
        }
      }
      append(token, &written, c);
    }
    return false;
  }

  bool read_long_string(Token *token) noexcept {
    return skip_long_bracket(long_bracket_level(m_cursor), token);
  }

  void read_symbol() noexcept {
    static constexpr const char *kLongSymbols[] = {
        "...", "..", "==", "~=", "<=", ">=", "//", "::", "<<", ">>"};
    for (const char *symbol : kLongSymbols) {
      const std::size_t length = std::strlen(symbol);
      if ((static_cast<std::size_t>(m_end - m_cursor) >= length) &&
          (std::memcmp(m_cursor, symbol, length) == 0)) {
        m_cursor += length;
        return;
      }
    }
    ++m_cursor;
  }

  const char *m_source;
  const char *m_end;
  const char *m_cursor = m_source;
  std::uint32_t m_line = 1U;
  bool m_error = false;
};

bool token_is(const Token &token, TokenKind kind, const char *text) noexcept {
  return (token.kind == kind) && (std::strlen(text) == token.length) &&
         (std::memcmp(token.begin, text, token.length) == 0);
}

bool is_symbol(const Token &token, const char *text) noexcept {
  return token_is(token, TokenKind::Symbol, text);
}

bool is_name(const Token &token, const char *text) noexcept {
  return token_is(token, TokenKind::Name, text);
}

/// Copies a token's spelling into `out`; false when it does not fit.
bool copy_spelling(const Token &token, char *out,
                   std::size_t capacity) noexcept {
  if (token.length + 1U > capacity) {
    return false;
  }
  std::memcpy(out, token.begin, token.length);
  out[token.length] = '\0';
  return true;
}

/// Records one diagnostic, counting it when the schema is full.
void diagnose(ScriptPropertySchema *out, std::uint32_t line, const char *format,
              ...) noexcept {
  if (out->diagnosticCount >= ScriptPropertySchema::kMaxDiagnostics) {
    ++out->droppedDiagnostics;
    return;
  }
  ScriptPropertyDiagnostic &entry = out->diagnostics[out->diagnosticCount++];
  entry.line = line;
  va_list args;
  va_start(args, format);
  static_cast<void>(
      std::vsnprintf(entry.message, sizeof(entry.message), format, args));
  va_end(args);
}

/// A number literal's value and whether Lua reads it as a float.
struct NumberLiteral final {
  double value = 0.0;
  std::int64_t integer = 0;
  bool isFloat = false;
  bool ok = false;
};

NumberLiteral parse_number(const Token &token, bool negative) noexcept {
  NumberLiteral number{};
  char text[64] = {};
  if (!copy_spelling(token, text, sizeof(text))) {
    return number;
  }
  const bool hex = (text[0] == '0') && ((text[1] == 'x') || (text[1] == 'X'));
  if (hex) {
    // Hexadecimal integers only; a hex float is not read.
    if (std::strpbrk(text + 2, ".pP") != nullptr) {
      return number;
    }
    char *end = nullptr;
    const unsigned long long value = std::strtoull(text + 2, &end, 16);
    if ((end == text + 2) || (*end != '\0')) {
      return number;
    }
    // Lua wraps hexadecimal integers around 2^64.
    number.integer = static_cast<std::int64_t>(value);
    number.integer = negative ? -number.integer : number.integer;
    number.value = static_cast<double>(number.integer);
    number.ok = true;
    return number;
  }
  // A property stores a float, so a float literal is read as one; one
  // past float's range is not a value a property can hold.
  number.isFloat = std::strpbrk(text, ".eE") != nullptr;
  float parsed = 0.0F;
  if (number.isFloat) {
    if (!core::parse_float_token(text, &parsed)) {
      return number;
    }
    number.value =
        negative ? -static_cast<double>(parsed) : static_cast<double>(parsed);
    number.ok = true;
    return number;
  }
  char *end = nullptr;
  errno = 0;
  const long long value = std::strtoll(text, &end, 10);
  if ((end == text) || (*end != '\0')) {
    return number;
  }
  if (errno == ERANGE) {
    // A decimal integer past int64 is a float in Lua.
    number.isFloat = true;
    number.ok = core::parse_float_token(text, &parsed);
    number.value =
        negative ? -static_cast<double>(parsed) : static_cast<double>(parsed);
    return number;
  }
  number.integer = negative ? -static_cast<std::int64_t>(value)
                            : static_cast<std::int64_t>(value);
  number.value = static_cast<double>(number.integer);
  number.ok = true;
  return number;
}

/// A literal read from the token stream: what the declaration's default or
/// a metadata field holds.
struct Literal final {
  enum class Kind : std::uint8_t { None, Bool, Number, String };
  Kind kind = Kind::None;
  bool boolValue = false;
  NumberLiteral number{};
  char text[ScriptPropertyValue::kMaxTextLength + 1U] = {};
  bool textFits = true;
  bool textReadable = true;
};

/// Reads the property table and everything in it from a token stream.
class PropertyReader final {
public:
  PropertyReader(Lexer *lexer, ScriptPropertySchema *out) noexcept
      : m_lexer(lexer), m_out(out) {
    advance();
  }

  void scan() noexcept {
    // Block depth counts function/do/if/repeat bodies: a declaration
    // inside one is not top-level. Brace and paren depth say where in an
    // expression the scan is.
    int blockDepth = 0;
    int braceDepth = 0;
    int parenDepth = 0;
    Token previous{};
    while (m_token.kind != TokenKind::End) {
      if (m_token.kind == TokenKind::Error) {
        diagnose(m_out, m_token.line,
                 "unterminated string or comment; properties after line %u "
                 "were not read",
                 static_cast<unsigned>(m_token.line));
        return;
      }
      const Token current = m_token;
      if (is_name(current, "function") || is_name(current, "do") ||
          is_name(current, "if") || is_name(current, "repeat")) {
        ++blockDepth;
      } else if (is_name(current, "end") || is_name(current, "until")) {
        blockDepth = (blockDepth > 0) ? (blockDepth - 1) : 0;
      } else if (is_symbol(current, "{")) {
        ++braceDepth;
      } else if (is_symbol(current, "}")) {
        braceDepth = (braceDepth > 0) ? (braceDepth - 1) : 0;
      } else if (is_symbol(current, "(")) {
        ++parenDepth;
      } else if (is_symbol(current, ")")) {
        parenDepth = (parenDepth > 0) ? (parenDepth - 1) : 0;
      } else if (is_name(current, "properties")) {
        // `x.properties = {` as a statement, or `properties = {` as a field
        // of a table constructor; a local or global named properties is
        // neither.
        const bool asField = is_symbol(previous, ".") && (braceDepth == 0);
        const bool asTableField =
            (braceDepth > 0) &&
            (is_symbol(previous, "{") || is_symbol(previous, ",") ||
             is_symbol(previous, ";"));
        advance();
        if ((asField || asTableField) && is_symbol(m_token, "=")) {
          advance();
          if (is_symbol(m_token, "{")) {
            const bool topLevel =
                (blockDepth == 0) && (asField || (braceDepth == 1));
            read_declaration(current.line, topLevel);
            previous = Token{};
            previous.kind = TokenKind::Symbol;
            continue;
          }
        }
        previous = current;
        continue;
      }
      previous = current;
      advance();
    }
  }

private:
  void advance() noexcept { m_token = m_lexer->next(); }

  /// Reads `{ ... }` at the cursor, the declaration table found on `line`.
  void read_declaration(std::uint32_t line, bool topLevel) noexcept {
    if (!topLevel) {
      diagnose(m_out, line,
               "properties declared inside a function or a nested table are "
               "not read; declare them in the script's top-level table");
      skip_table();
      return;
    }
    if (m_out->declared) {
      diagnose(m_out, line,
               "a second properties table is not read; the first, on line "
               "%u, is",
               static_cast<unsigned>(m_firstLine));
      skip_table();
      return;
    }
    m_out->declared = true;
    m_firstLine = line;
    advance(); // past '{'
    while ((m_token.kind != TokenKind::End) &&
           (m_token.kind != TokenKind::Error) && !is_symbol(m_token, "}")) {
      read_entry();
      if (is_symbol(m_token, ",") || is_symbol(m_token, ";")) {
        advance();
      }
    }
    if (is_symbol(m_token, "}")) {
      advance();
    }
  }

  /// Reads one `name = value` entry, leaving the cursor on the separator or
  /// the closing brace.
  void read_entry() noexcept {
    const Token nameToken = m_token;
    if (nameToken.kind != TokenKind::Name) {
      diagnose(m_out, nameToken.line,
               "a property is declared as name = value; this entry is not");
      skip_value();
      return;
    }
    advance();
    if (!is_symbol(m_token, "=")) {
      diagnose(m_out, nameToken.line,
               "a property is declared as name = value; this entry is not");
      skip_value();
      return;
    }
    advance();
    ScriptPropertyDecl decl{};
    decl.line = nameToken.line;
    if (!copy_spelling(nameToken, decl.name, sizeof(decl.name)) ||
        !math::script_property_name_is_valid(decl.name)) {
      diagnose(m_out, nameToken.line,
               "property name longer than %u characters is not read",
               static_cast<unsigned>(math::kMaxScriptPropertyNameLength));
      skip_value();
      return;
    }
    bool ok = false;
    if (is_symbol(m_token, "{")) {
      ok = read_table_form(&decl);
    } else {
      Literal literal{};
      if (read_literal(&literal) && at_value_end()) {
        ok = literal_to_value(literal, decl.name, decl.line, nullptr,
                              &decl.defaultValue);
      } else {
        diagnose(m_out, decl.line,
                 "property '%s' is not a literal; its default must be a "
                 "number, string, true or false",
                 decl.name);
        skip_value();
      }
    }
    if (ok) {
      add(decl);
    }
  }

  /// Reads the `{ type = ..., default = ..., ... }` form.
  bool read_table_form(ScriptPropertyDecl *decl) noexcept {
    advance(); // past '{'
    bool hasType = false;
    ScriptPropertyType type = ScriptPropertyType::Float;
    Literal defaultLiteral{};
    bool hasDefault = false;
    bool ok = true;
    while ((m_token.kind != TokenKind::End) &&
           (m_token.kind != TokenKind::Error) && !is_symbol(m_token, "}")) {
      const Token key = m_token;
      advance();
      if ((key.kind != TokenKind::Name) || !is_symbol(m_token, "=")) {
        diagnose(m_out, key.line,
                 "property '%s': each field is written key = value",
                 decl->name);
        skip_value();
      } else {
        advance();
        Literal literal{};
        const bool readable = read_literal(&literal) && at_value_end();
        if (!readable) {
          diagnose(m_out, key.line,
                   "property '%s': field '%.*s' is not a literal", decl->name,
                   static_cast<int>(key.length), key.begin);
          skip_value();
          // Only the type and the default decide whether the property can
          // be shown; a bad tooltip or range is reported and left out.
          ok = ok && !is_name(key, "type") && !is_name(key, "default");
        } else if (is_name(key, "type")) {
          hasType = true;
          ok = read_type(literal, decl, &type) && ok;
        } else if (is_name(key, "default")) {
          hasDefault = true;
          defaultLiteral = literal;
        } else if (is_name(key, "min") || is_name(key, "max")) {
          if (literal.kind != Literal::Kind::Number) {
            diagnose(m_out, key.line, "property '%s': %.*s is not a number",
                     decl->name, static_cast<int>(key.length), key.begin);
          } else if (is_name(key, "min")) {
            decl->hasMin = true;
            decl->minValue = literal.number.value;
          } else {
            decl->hasMax = true;
            decl->maxValue = literal.number.value;
          }
        } else if (is_name(key, "tooltip")) {
          if (literal.kind != Literal::Kind::String) {
            diagnose(m_out, key.line, "property '%s': tooltip is not a string",
                     decl->name);
          } else {
            std::snprintf(decl->tooltip, sizeof(decl->tooltip), "%s",
                          literal.text);
          }
        } else {
          diagnose(m_out, key.line,
                   "property '%s': unknown field '%.*s' (type, default, min, "
                   "max and tooltip are read)",
                   decl->name, static_cast<int>(key.length), key.begin);
        }
      }
      if (is_symbol(m_token, ",") || is_symbol(m_token, ";")) {
        advance();
      }
    }
    if (is_symbol(m_token, "}")) {
      advance();
    }
    if (!ok) {
      return false;
    }
    if (!hasType && !hasDefault) {
      diagnose(m_out, decl->line,
               "property '%s' names neither a type nor a default", decl->name);
      return false;
    }
    if (!hasDefault) {
      decl->defaultValue = ScriptPropertyValue{};
      decl->defaultValue.type = type;
      return true;
    }
    return literal_to_value(defaultLiteral, decl->name, decl->line,
                            hasType ? &type : nullptr, &decl->defaultValue);
  }

  bool read_type(const Literal &literal, const ScriptPropertyDecl *decl,
                 ScriptPropertyType *out) noexcept {
    if (literal.kind == Literal::Kind::String) {
      for (std::uint32_t i = 0U; i < math::kScriptPropertyTypeCount; ++i) {
        const auto type = static_cast<ScriptPropertyType>(i);
        if (std::strcmp(literal.text, script_property_type_name(type)) == 0) {
          *out = type;
          return true;
        }
      }
    }
    diagnose(m_out, decl->line,
             "property '%s': type is one of \"bool\", \"integer\", \"float\" "
             "and \"string\"",
             decl->name);
    return false;
  }

  /// Converts a literal to a value of `*type`, or of the literal's own
  /// type when `type` is null.
  bool literal_to_value(const Literal &literal, const char *name,
                        std::uint32_t line, const ScriptPropertyType *type,
                        ScriptPropertyValue *out) noexcept {
    ScriptPropertyValue value{};
    switch (literal.kind) {
    case Literal::Kind::Bool:
      value.type = ScriptPropertyType::Bool;
      value.boolValue = literal.boolValue;
      break;
    case Literal::Kind::Number:
      if (literal.number.isFloat) {
        value.type = ScriptPropertyType::Float;
        value.floatValue = static_cast<float>(literal.number.value);
      } else {
        value.type = ScriptPropertyType::Integer;
        value.integerValue = literal.number.integer;
      }
      break;
    case Literal::Kind::String:
      if (!literal.textFits || !literal.textReadable) {
        diagnose(m_out, line,
                 literal.textFits
                     ? "property '%s': the string uses an escape the editor "
                       "does not read"
                     : "property '%s': the string is longer than %u bytes",
                 name,
                 static_cast<unsigned>(ScriptPropertyValue::kMaxTextLength));
        return false;
      }
      value.type = ScriptPropertyType::String;
      std::memcpy(value.text, literal.text, sizeof(value.text));
      break;
    case Literal::Kind::None:
      return false;
    }
    if ((type != nullptr) && (*type != value.type)) {
      // An integer literal is a fine default for a float property.
      if ((*type == ScriptPropertyType::Float) &&
          (value.type == ScriptPropertyType::Integer)) {
        const auto integer = value.integerValue;
        value = ScriptPropertyValue{};
        value.type = ScriptPropertyType::Float;
        value.floatValue = static_cast<float>(integer);
      } else {
        diagnose(m_out, line, "property '%s': the default is not a %s", name,
                 script_property_type_name(*type));
        return false;
      }
    }
    if (!math::script_property_value_is_valid(value)) {
      diagnose(m_out, line, "property '%s': the default is out of range", name);
      return false;
    }
    *out = value;
    return true;
  }

  /// Reads a literal at the cursor, advancing past it; false, with the
  /// cursor unmoved past what it read, for anything else.
  bool read_literal(Literal *out) noexcept {
    if (is_name(m_token, "true") || is_name(m_token, "false")) {
      out->kind = Literal::Kind::Bool;
      out->boolValue = is_name(m_token, "true");
      advance();
      return true;
    }
    if (m_token.kind == TokenKind::String) {
      out->kind = Literal::Kind::String;
      std::memcpy(out->text, m_token.text, sizeof(out->text));
      out->textFits = m_token.textFits;
      out->textReadable = m_token.textReadable;
      advance();
      return true;
    }
    bool negative = false;
    if (is_symbol(m_token, "-")) {
      negative = true;
      advance();
    }
    if (m_token.kind == TokenKind::Number) {
      out->number = parse_number(m_token, negative);
      advance();
      if (!out->number.ok) {
        return false;
      }
      out->kind = Literal::Kind::Number;
      return true;
    }
    return false;
  }

  /// True when the cursor ends a value: a separator or the closing brace.
  bool at_value_end() const noexcept {
    return is_symbol(m_token, ",") || is_symbol(m_token, ";") ||
           is_symbol(m_token, "}");
  }

  /// Skips the rest of a value, leaving the cursor on the separator or
  /// closing brace that ends it.
  void skip_value() noexcept {
    int depth = 0;
    while ((m_token.kind != TokenKind::End) &&
           (m_token.kind != TokenKind::Error)) {
      if (is_symbol(m_token, "{") || is_symbol(m_token, "(") ||
          is_symbol(m_token, "[")) {
        ++depth;
      } else if (is_symbol(m_token, "}") || is_symbol(m_token, ")") ||
                 is_symbol(m_token, "]")) {
        if (depth == 0) {
          return;
        }
        --depth;
      } else if ((depth == 0) &&
                 (is_symbol(m_token, ",") || is_symbol(m_token, ";"))) {
        return;
      }
      advance();
    }
  }

  /// Skips a whole `{ ... }` at the cursor.
  void skip_table() noexcept {
    advance();
    skip_value();
    while (!is_symbol(m_token, "}") && (m_token.kind != TokenKind::End) &&
           (m_token.kind != TokenKind::Error)) {
      advance();
      skip_value();
    }
    if (is_symbol(m_token, "}")) {
      advance();
    }
  }

  void add(const ScriptPropertyDecl &decl) noexcept {
    if (find_script_property(*m_out, decl.name) != nullptr) {
      diagnose(m_out, decl.line,
               "property '%s' is declared twice; the first "
               "declaration is read",
               decl.name);
      return;
    }
    if (m_out->count >= ScriptPropertySchema::kMaxProperties) {
      diagnose(m_out, decl.line,
               "property '%s' is past the %u a script can declare", decl.name,
               static_cast<unsigned>(ScriptPropertySchema::kMaxProperties));
      return;
    }
    m_out->properties[m_out->count++] = decl;
  }

  Lexer *m_lexer;
  ScriptPropertySchema *m_out;
  Token m_token{};
  std::uint32_t m_firstLine = 0U;
};

} // namespace

void scan_script_properties(const char *source, std::size_t length,
                            ScriptPropertySchema *out) noexcept {
  if (out == nullptr) {
    return;
  }
  *out = ScriptPropertySchema{};
  if ((source == nullptr) || (length == 0U)) {
    return;
  }
  Lexer lexer(source, length);
  PropertyReader reader(&lexer, out);
  reader.scan();
}

const ScriptPropertyDecl *
find_script_property(const ScriptPropertySchema &schema,
                     const char *name) noexcept {
  if (name == nullptr) {
    return nullptr;
  }
  for (std::size_t i = 0U; i < schema.count; ++i) {
    if (std::strcmp(schema.properties[i].name, name) == 0) {
      return &schema.properties[i];
    }
  }
  return nullptr;
}

const char *script_property_type_name(math::ScriptPropertyType type) noexcept {
  switch (type) {
  case math::ScriptPropertyType::Bool:
    return "bool";
  case math::ScriptPropertyType::Integer:
    return "integer";
  case math::ScriptPropertyType::Float:
    return "float";
  case math::ScriptPropertyType::String:
    return "string";
  }
  return "unknown";
}

} // namespace engine::scripting
