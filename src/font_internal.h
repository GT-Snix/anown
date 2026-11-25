// Internal: fonts and the glyph cache (docs/text.md). A font belongs to one
// scene and lives in its arena. Glyph outlines are flattened once per
// (font, glyph), in font units, the first time an object uses the glyph
// (always at script time, never while rendering: rule 10).
#ifndef ANOWN_FONT_INTERNAL_H
#define ANOWN_FONT_INTERNAL_H

#include "anown.h"
#include "arena_internal.h"
#include "path_internal.h"

#include "stb_truetype.h"

// Flattening tolerance, as a fraction of the em: 0.075 px at 300 px.
#define ANI_GLYPH_FLATNESS_EM (1.0f / 4000.0f)

typedef struct ani_glyph {
    int index;            // glyph index in the font
    int advance, lsb;     // font units
    int x0, y0, x1, y1;   // bounding box, font units, y UP (as in the font)
    ani_path *path;       // contours, font units, y DOWN (y negated); NULL if none
} ani_glyph;

struct an_font {
    an_scene *scene;
    stbtt_fontinfo info;
    const unsigned char *data; // the font file (arena copy, or built-in bytes)
    int upem;                  // units per em
    int ascent, descent, line_gap; // hhea, font units (descent negative)
    int stem;                  // H stem width, font units (measured from the outline)
    int x_height;              // top of 'x', font units
    int num_glyphs;
    ani_glyph **cache;         // num_glyphs slots, filled on first use
    const char *name;          // for messages
};

// Cached glyph (flattened on first use). NULL only on allocation failure.
const ani_glyph *ani_font_glyph(an_font *f, int glyph_index);
// Glyph index of a codepoint (0 = .notdef if the font lacks it).
int ani_font_index(const an_font *f, uint32_t codepoint);
// Kerning between two glyphs in font units (kern table and GPOS pairs).
int ani_font_kern(const an_font *f, int g1, int g2);

#endif
