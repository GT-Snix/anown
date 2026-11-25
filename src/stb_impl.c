// The one translation unit that holds the stb implementations.
// Vendor code: built without -Werror (see Makefile), like sokol_impl.m.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// stb_truetype, for glyph outlines and metrics (src/font.c).
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
