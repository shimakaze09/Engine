// Compiles miniaudio's implementation, and with it the set of formats the
// engine decodes. WAV, FLAC and MP3 are miniaudio's own; Ogg Vorbis comes
// from the stb_vorbis decoder in the same pinned miniaudio checkout, which
// miniaudio uses when it is included around the implementation. The
// checks at the end hold this set to kLoadableSoundExtensions in audio.h.

#include "engine/audio/audio.h"

#include <iterator>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wtautological-constant-out-of-range-compare"
#pragma clang diagnostic ignored "-Wtautological-compare"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wunused-value"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
// miniaudio's Emscripten backend uses EM_JS ($-identifiers), legacy
// version macros, and unused callback params.
#pragma clang diagnostic ignored "-Wdollar-in-identifier-extension"
#pragma clang diagnostic ignored "-Wdeprecated-pragma"
#pragma clang diagnostic ignored "-Wunused-parameter"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-result"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

#define STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#undef STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#if !defined(MA_HAS_WAV) || !defined(MA_HAS_FLAC) || !defined(MA_HAS_MP3) ||   \
    !defined(MA_HAS_VORBIS)
#error "miniaudio is built without a decoder kLoadableSoundExtensions names"
#endif
static_assert(std::size(engine::audio::kLoadableSoundExtensions) == 4U,
              "a format added to kLoadableSoundExtensions needs its decoder "
              "checked above");
