// Script-time bounds (docs/explainer.md, "Bounds"): boxes around geometry at the
// cursor, from the tracks recorded so far. Pure: sampling only.
#include "scene_internal.h"
#include "tess_internal.h"
#include "timeline_internal.h"

#include <math.h>

static void add_obj(an_bounds *b, an_obj *o, ani_time at) {
    if (!ani_obj_exists(o, at)) return;
    if (o->path && o->path->npts > 0) {
        const ani_affine w = ani_obj_world_at(o, at);
        float hw = 0.0f; // half the ink width on screen
        if (o->has_stroke) {
            const double sw = ani_sample(ani_obj_prop(o, AN_PROP_STROKE_WIDTH), at).v[0];
            hw = (float)sw * sqrtf(fabsf(w.a * w.d - w.b * w.c)) * 0.5f;
        }
        for (int k = 0; k < o->path->npts; k++) {
            const an_vec2 p = ani_affine_apply(w, o->path->pts[k]);
            b->min.x = fminf(b->min.x, p.x - hw);
            b->min.y = fminf(b->min.y, p.y - hw);
            b->max.x = fmaxf(b->max.x, p.x + hw);
            b->max.y = fmaxf(b->max.y, p.y + hw);
        }
    }
    for (an_obj *c = o->first_child; c; c = c->next_sibling) add_obj(b, c, at);
}

an_bounds an_obj_bounds_at_cursor(const an_obj *o) {
    an_bounds b = { { INFINITY, INFINITY }, { -INFINITY, -INFINITY } };
    if (!o) return b;
    add_obj(&b, (an_obj *)o, ani_tick_time(o->scene->cursor));
    return b;
}
