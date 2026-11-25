// Callouts and array pointers (docs/explainer.md): a label and a curved arrow
// in a group. Geometry is fixed at creation (rule 10); the group moves.
#include "font_internal.h"
#include "scene_internal.h"
#include "timeline_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

// The label's advance width (first line) at `size`, from the font's metrics.
static float label_width(an_font *f, const char *str, float size) {
    const float k = size / (float)f->upem;
    float w = 0.0f;
    int prev = -1;
    for (const unsigned char *p = (const unsigned char *)str; *p && *p != '\n'; p++) {
        if ((*p & 0xC0) == 0x80) continue; // UTF-8 continuation bytes: ASCII labels are the use here
        const int gi = ani_font_index(f, *p);
        const ani_glyph *g = ani_font_glyph(f, gi);
        if (!g) break;
        if (prev >= 0) w += (float)ani_font_kern(f, prev, gi) * k;
        w += (float)g->advance * k;
        prev = gi;
    }
    return w;
}

an_obj *an_callout(an_scene *s, const an_callout_opts *opts) {
    const an_callout_opts o = opts ? *opts : (an_callout_opts){ 0 };
    if (!s) return NULL;
    const an_palette *pal = s->style.palette;
    an_font *f = o.font ? o.font : an_font_display(s);
    if (!f) return NULL;
    const float size = o.size > 0.0f ? o.size : 34.0f;
    const float gap = o.gap > 0.0f ? o.gap : 10.0f;
    const char *text = o.text ? o.text : "";
    an_obj *g = ani_obj_make(s, AN_KIND_CALLOUT, &(ani_common_opts){
        .name = o.name, .hidden = o.hidden, .parent = o.parent, .pos = o.pos, .scale = o.scale,
        .rotation = o.rotation, .z = o.z });
    if (!g) return NULL;
    an_obj *label = an_text(s, &(an_text_opts){ .parent = g, .text = text, .font = f, .size = size,
                                                 .align = AN_ALIGN_CENTER, .anchor = AN_ANCHOR_MIDDLE,
                                                 .fill = o.fill.a > 0.0f ? o.fill : pal->ink, .undrawn = o.undrawn });
    if (!label) return NULL;
    // the target relative to the label's center (the group's origin)
    const an_vec2 t = { o.target.x - o.pos.x, o.target.y - o.pos.y };
    const float tl = sqrtf(t.x * t.x + t.y * t.y);
    if (tl > 1e-3f) {
        // start: where the ray toward the target leaves the label's box, plus 8 px
        const float hw = label_width(f, text, size) * 0.5f + 8.0f;
        const float hh = (float)(f->ascent - f->descent) * size / (float)f->upem * 0.5f + 8.0f;
        const float sx = fabsf(t.x) > 1e-6f ? hw / fabsf(t.x) : INFINITY, sy = fabsf(t.y) > 1e-6f ? hh / fabsf(t.y) : INFINITY;
        const float k = fminf(fminf(sx, sy), 0.9f);
        const an_vec2 a0 = { t.x * k, t.y * k };
        // control: the chord's midpoint, pushed along the up-screen normal by bend x half the chord
        const float dx = t.x - a0.x, dy = t.y - a0.y, len = sqrtf(dx * dx + dy * dy);
        float nx = dy / len, ny = -dx / len;
        if (ny > 0.0f || (ny == 0.0f && nx > 0.0f)) {
            nx = -nx;
            ny = -ny;
        }
        const float bend = o.bend < -1.0f ? -1.0f : o.bend > 1.0f ? 1.0f : o.bend;
        const an_vec2 c = { (a0.x + t.x) * 0.5f + nx * bend * len * 0.5f, (a0.y + t.y) * 0.5f + ny * bend * len * 0.5f };
        // end: gap short of the target along the end tangent (target - control)
        const float ex = t.x - c.x, ey = t.y - c.y, el = sqrtf(ex * ex + ey * ey);
        const an_vec2 e = { t.x - ex / el * gap, t.y - ey / el * gap };
        // head: two strokes back from the tip at +-28 degrees, 16 px
        const float ux = ex / el, uy = ey / el, cs = cosf(0.4887f), sn = sinf(0.4887f), hl = 16.0f;
        const an_vec2 h1 = { e.x - hl * (ux * cs - uy * sn), e.y - hl * (uy * cs + ux * sn) };
        const an_vec2 h2 = { e.x - hl * (ux * cs + uy * sn), e.y - hl * (uy * cs - ux * sn) };
        an_obj *arrow = an_stroke(s, &(an_stroke_opts){
            .parent = g, .undrawn = o.undrawn, .stroke = o.stroke.a > 0.0f ? o.stroke : pal->ink,
            .stroke_width = o.stroke_width > 0.0f ? o.stroke_width : 5.0f, .brush = o.brush, .shadow = o.shadow,
            .segs = (an_seg[]){ AN_MOVE(a0.x, a0.y), AN_QUAD(c.x, c.y, e.x, e.y), AN_MOVE(h1.x, h1.y), AN_LINE(e.x, e.y),
                                AN_LINE(h2.x, h2.y) },
            .seg_count = 5 });
        if (!arrow) return NULL;
        g->u.callout.arrow = arrow;
    }
    g->u.callout.label = label;
    return g;
}

an_anim an_callout_draw(an_obj *c, const an_anim_opts *opts) {
    an_anim a = { .obj = c, .verb = AN_VERB_CALLOUT_DRAW };
    if (opts) a.opts = *opts;
    return a;
}

int64_t ani_callout_record_draw(an_scene *s, const an_anim *a, int64_t start) {
    an_obj *c = a->obj;
    if (c->kind != AN_KIND_CALLOUT) return start;
    int64_t end = start;
    if (c->u.callout.arrow) {
        const an_anim d = an_draw(c->u.callout.arrow, &a->opts);
        const int64_t e = ani_record_anim(s, &d, start);
        end = e > end ? e : end;
    }
    const an_anim d = an_draw(c->u.callout.label, &a->opts);
    const int64_t e = ani_record_anim(s, &d, start);
    return e > end ? e : end;
}

// ---- array pointers ------------------------------------------------------------------------

an_obj *an_array_pointer(an_obj *arr, const char *label, int slot) {
    if (!arr || arr->kind != AN_KIND_ARRAY || slot < 0 || slot >= arr->u.array.n) return NULL;
    const bool bars = arr->u.array.style == AN_ARRAY_BARS;
    const an_vec2 cell = arr->u.array.cell;
    // below the row and its index labels: the arrow tip just under the labels
    const float below = bars ? cell.x * 1.3f : cell.y * 0.5f + cell.y * 0.75f;
    const float x = arr->u.array.slot_x[slot];
    return an_callout(arr->scene, &(an_callout_opts){
        .parent = arr, .text = label, .size = bars ? cell.x * 0.6f : cell.y * 0.36f, .font = an_font_mono(arr->scene),
        .pos = { x, below + (bars ? cell.x * 1.4f : cell.y * 0.95f) }, .target = { x, below }, .gap = 4,
        .stroke_width = 4 });
}

an_anim an_array_point(an_obj *ptr, int slot, const an_anim_opts *opts) {
    an_obj *arr = ptr ? ptr->parent : NULL;
    if (!arr || arr->kind != AN_KIND_ARRAY || slot < 0 || slot >= arr->u.array.n) return (an_anim){ 0 };
    an_anim_opts o = opts ? *opts : (an_anim_opts){ 0 };
    if (!(o.duration > 0.0f)) o.duration = 0.35f;
    return an_move_to(ptr, (an_vec2){ arr->u.array.slot_x[slot], ptr->pos.y }, &o);
}
