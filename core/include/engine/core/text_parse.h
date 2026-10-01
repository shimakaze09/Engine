// Parses whole text tokens into numbers, strictly: the one parser a cvar
// value, a stored preference and a console argument share, so a value one
// of them accepts the others accept too.
#pragma once

namespace engine::core {

/// Parses `text` as one finite float: the whole token, with no leading
/// space, no trailing text, no overflow and no inf or nan spelling. False,
/// with `*out` unchanged, for anything else, a null or empty `text`
/// included.
bool parse_float_token(const char *text, float *out) noexcept;

} // namespace engine::core
