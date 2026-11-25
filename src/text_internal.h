// Internal: text helpers shared by the tessellator and the timeline
// (docs/text.md).
#ifndef ANOWN_TEXT_INTERNAL_H
#define ANOWN_TEXT_INTERNAL_H

#include "tess_internal.h"

// The chaos size factor of a letter whose world transform scales by `wscale`
// (sqrt |det|): min(clamp((stem - 3) / 5, 0, 1), 0.3 stem / (2 boil)), stem in
// px on screen. 1 for anything that isn't a letter, or when the text has it off.
// boil: the style's boil amplitude (0: no cap).
float ani_text_size_factor(const an_obj *glyph, float wscale, float boil);
// Riso offset multiplier for a letter: clamp(em px on screen / 100, 0.5, 3).
float ani_text_riso_scale(const an_obj *glyph, float wscale);
// Pixel snapping relative to the text's origin: when the effective scale is
// exactly 1 (the parent (text) world transform pw is a pure translation, and the
// letter isn't rotated or scaled), shift w so that the pen offset lands on whole
// pixels. Returns whether it snapped.
bool ani_text_snap(ani_affine pw, ani_affine *w, an_vec2 pen, float rotation, an_vec2 scale);

#endif
