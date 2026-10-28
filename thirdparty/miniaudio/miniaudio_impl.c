/* miniaudio's implementation (reNut's stereo music, src/renut_engine/game/stereo.cpp),
 * with stb_vorbis for Ogg Vorbis files, as miniaudio's documentation sets it up. */
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#undef STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
