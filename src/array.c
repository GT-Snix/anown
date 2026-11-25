// The array / bar visualizer (docs/explainer.md): a group of element groups
// (cell or bar + value label) and index labels; swaps as arc + slide tracks.
// Script-time bookkeeping (the slot permutation) is updated when a swap is
// played and never read while rendering.
#include "scene_internal.h"
#include "tess_internal.h"
#include "timeline_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static an_color mix(an_color a, an_color b, float t) {
    return (an_color){ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1.0f };
}

an_obj *an_array(an_scene *s, const an_array_opts *opts) {
    const an_array_opts o = opts ? *opts : (an_array_opts){ 0 };
    if (!s || !o.values || o.count <= 0) return NULL;
    const an_palette *pal = s->style.palette;
    const bool bars = o.style == AN_ARRAY_BARS;
    const an_vec2 cell = (o.cell.x > 0 && o.cell.y > 0) ? o.cell : bars ? (an_vec2){ 56, 320 } : (an_vec2){ 96, 96 };
    const float gap = o.gap > 0.0f ? o.gap : (bars ? 12.0f : 0.0f);
    const int n = o.count;
    an_font *font = o.font ? o.font : an_font_mono(s);
    if (!font) return NULL;

    an_obj *a = ani_obj_make(s, AN_KIND_ARRAY, &(ani_common_opts){
        .name = o.name, .hidden = o.hidden, .parent = o.parent, .pos = o.pos, .scale = o.scale,
        .rotation = o.rotation, .z = o.z });
    if (!a) return NULL;
    a->u.array.n = n;
    a->u.array.elems = ani_arena_alloc(&s->arena, sizeof(an_obj *) * (size_t)n, alignof(an_obj *));
    a->u.array.cells = ani_arena_alloc(&s->arena, sizeof(an_obj *) * (size_t)n, alignof(an_obj *));
    a->u.array.perm = ani_arena_alloc(&s->arena, sizeof(int) * (size_t)n, alignof(int));
    a->u.array.busy = ani_arena_alloc(&s->arena, sizeof(int64_t) * (size_t)n, alignof(int64_t));
    a->u.array.slot_x = ani_arena_alloc(&s->arena, sizeof(float) * (size_t)n, alignof(float));
    a->u.array.height = ani_arena_alloc(&s->arena, sizeof(float) * (size_t)n, alignof(float));
    if (!a->u.array.elems || !a->u.array.cells || !a->u.array.perm || !a->u.array.busy || !a->u.array.slot_x ||
        !a->u.array.height)
        return NULL;
    a->u.array.cell = cell;
    a->u.array.style = o.style;
    const an_color fill = o.fill.a > 0.0f ? o.fill : pal->a;
    a->u.array.fill = fill;
    const an_color defaults[4] = { fill, pal->b, mix(s->background, pal->ink, 0.12f), pal->c };
    for (int k = 0; k < 4; k++) a->u.array.states[k] = o.state_colors[k].a > 0.0f ? o.state_colors[k] : defaults[k];
    int vmax = 1;
    for (int k = 0; k < n; k++) vmax = o.values[k] > vmax ? o.values[k] : vmax;
    // cells with a pattern fill get paper under them by default
    const an_toggle pu = o.paper_under != AN_AUTO ? o.paper_under
                         : (o.pattern.kind != AN_PATTERN_SOLID ? AN_ON : AN_OFF);
    const float value_size = bars ? cell.x * 0.62f : cell.y * 0.42f;
    const float index_size = bars ? cell.x * 0.5f : cell.y * 0.3f;
    for (int k = 0; k < n; k++) {
        const float x = ((float)k - (float)(n - 1) * 0.5f) * (cell.x + gap);
        a->u.array.slot_x[k] = x;
        a->u.array.perm[k] = k;
        an_obj *e = an_group(s, &(an_group_opts){ .parent = a, .pos = { x, 0 } });
        if (!e) return NULL;
        float h = cell.y;
        an_vec2 rc = { 0, 0 };
        if (bars) {
            const int v = o.values[k] > 0 ? o.values[k] : 0;
            h = cell.y * (float)v / (float)vmax;
            if (h < 4.0f) h = 4.0f;
            rc = (an_vec2){ 0, -h * 0.5f }; // bottoms on the baseline (y = 0)
        }
        a->u.array.height[k] = h;
        an_obj *r = an_rect(s, &(an_rect_opts){
            .parent = e, .pos = rc, .size = { cell.x, h }, .fill = fill,
            .stroke = o.stroke.a > 0.0f ? o.stroke : pal->ink, .stroke_width = o.stroke_width > 0.0f ? o.stroke_width : 5.0f,
            .pattern = o.pattern, .riso = o.riso, .shadow = o.shadow, .paper_under = pu, .brush = o.brush });
        if (!r) return NULL;
        a->u.array.elems[k] = e;
        a->u.array.cells[k] = r;
        if (!o.no_values) {
            char lab[24];
            snprintf(lab, sizeof lab, "%d", o.values[k]);
            an_obj *t = an_text(s, &(an_text_opts){
                .parent = e, .text = lab, .font = font, .size = value_size, .align = AN_ALIGN_CENTER,
                .anchor = bars ? AN_ANCHOR_BOTTOM : AN_ANCHOR_MIDDLE,
                .pos = { 0, bars ? -h - value_size * 0.25f : 0 }, .fill = pal->ink });
            if (!t) return NULL;
        }
        if (o.index_labels) {
            char lab[24];
            snprintf(lab, sizeof lab, "%d", k);
            const float y = bars ? index_size * 0.4f : cell.y * 0.5f + index_size * 0.45f;
            an_obj *t = an_text(s, &(an_text_opts){
                .parent = a, .text = lab, .font = font, .size = index_size, .align = AN_ALIGN_CENTER,
                .anchor = AN_ANCHOR_TOP, .pos = { x, y }, .fill = mix(pal->ink, s->background, 0.45f) });
            if (!t) return NULL;
        }
    }
    return a;
}

static bool valid(const an_obj *a, int slot) {
    return a && a->kind == AN_KIND_ARRAY && slot >= 0 && slot < a->u.array.n;
}

an_obj *an_array_item(const an_obj *a, int slot) {
    return valid(a, slot) ? a->u.array.elems[a->u.array.perm[slot]] : NULL;
}

an_obj *an_array_cell(const an_obj *a, int slot) {
    return valid(a, slot) ? a->u.array.cells[a->u.array.perm[slot]] : NULL;
}

an_vec2 an_array_slot_pos(const an_obj *a, int slot) {
    return valid(a, slot) ? (an_vec2){ a->u.array.slot_x[slot], 0 } : (an_vec2){ 0, 0 };
}

an_anim an_array_highlight(an_obj *a, int slot, an_color fill, const an_anim_opts *opts) {
    return an_fill_to(an_array_cell(a, slot), fill, opts);
}

an_anim an_array_unhighlight(an_obj *a, int slot, const an_anim_opts *opts) {
    return an_fill_to(an_array_cell(a, slot), valid(a, slot) ? a->u.array.fill : AN_BLACK, opts);
}

// The swap's arc heights (array units): H for the element arcing over (A, from
// slot i), HB for B (0 for adjacent slots: it slides), the lift (eased progress
// before anything travels) and h, B's height (docs/explainer.md).
static void swap_heights(const an_obj *a, int i, int j, float *H_out, float *HB_out, double *lift_out, float *h_out) {
    const float xi = a->u.array.slot_x[i], xj = a->u.array.slot_x[j];
    const float d = fabsf(xj - xi);
    const bool bars = a->u.array.style == AN_ARRAY_BARS;
    const int eb = a->u.array.perm[j];
    // how high an element has to rise to clear a neighbor: a cell, or the tallest
    // bar (plus its label) it passes over
    const float label = bars ? a->u.array.cell.x * 0.8f : 0.0f;
    float h = bars ? a->u.array.height[eb] + label : a->u.array.cell.y;
    const int lo = i < j ? i : j, hi = i < j ? j : i;
    float h_mid = 0.0f; // the tallest element between the two slots
    for (int k = lo + 1; k < hi; k++) {
        const float hk = bars ? a->u.array.height[a->u.array.perm[k]] + label : a->u.array.cell.y;
        h_mid = hk > h_mid ? hk : h_mid;
    }
    const bool adjacent = hi - lo == 1;
    // Lifted arcs (docs/explainer.md): an element rises straight up until it's
    // clear of its neighbors and only then travels, so nothing touches.
    // Adjacent: A arcs over (H), B slides under while A is >= h up. Otherwise both
    // arc above: B at H_B (clears the elements in between), A higher by the
    // separation needed where their paths cross.
    float HB = 0.0f, H;
    double lift; // the eased progress before which nothing travels
    if (adjacent) {
        H = fmaxf(1.5f * h, 0.35f * d);
        lift = (1.0 - sqrt(1.0 - (double)h / (double)H)) / 2.0;
    } else {
        HB = fmaxf(1.5f * h_mid, 0.25f * d);
        lift = (1.0 - sqrt(1.0 - (double)h_mid / (double)HB)) / 2.0;
        // A and B overlap horizontally while |1 - 2 S(k)| < w / d (S: smoothstep of
        // the travel phase k); from k1 with S(k1) = (1 - w/d) / 2, i.e. eased
        // progress u1 = lift + k1 (1 - 2 lift). Their vertical gap there,
        // (H - HB) 4 u1 (1 - u1), must be >= B's height h.
        const double target = (1.0 - (double)a->u.array.cell.x / (double)d) / 2.0;
        double lo_k = 0.0, hi_k = 0.5;
        for (int it = 0; it < 60; it++) {
            const double m = (lo_k + hi_k) / 2.0;
            if (m * m * (3.0 - 2.0 * m) < target) lo_k = m; else hi_k = m;
        }
        const double u1 = lift + lo_k * (1.0 - 2.0 * lift);
        H = HB + (float)((double)h / (4.0 * u1 * (1.0 - u1))) * 1.05f;
    }
    *H_out = H;
    *HB_out = HB;
    *lift_out = lift;
    *h_out = h;
}

an_anim an_array_swap(an_obj *a, int i, int j, const an_anim_opts *opts) {
    an_anim x = { .obj = a, .verb = AN_VERB_SWAP, .swap_i = i, .swap_j = j };
    if (opts) x.opts = *opts;
    return x;
}

// The swap (docs/explainer.md, "Swap: arcs above the row").
int64_t ani_array_record_swap(an_scene *s, const an_anim *x, int64_t start) {
    an_obj *a = x->obj;
    const int i = x->swap_i, j = x->swap_j;
    if (!valid(a, i) || !valid(a, j) || i == j) return start;
    an_obj *A = an_array_item(a, i), *B = an_array_item(a, j);
    const int ea = a->u.array.perm[i], eb = a->u.array.perm[j];
    const float xi = a->u.array.slot_x[i], xj = a->u.array.slot_x[j];
    const bool bars = a->u.array.style == AN_ARRAY_BARS;
    float HB, H;
    double lift;
    float h;
    swap_heights(a, i, j, &H, &HB, &lift, &h);
    const bool adjacent = abs(i - j) == 1;
    const float hmin = H;

    // clamp to the frame: the row's top on screen at the cursor (base + tracks, camera)
    const ani_time at = ani_tick_time(start);
    const ani_affine w = ani_obj_world_at(a, at);
    const ani_camera cam = ani_camera_at(s, at);
    const float top_local = bars ? -a->u.array.height[ea] : -a->u.array.cell.y * 0.5f;
    const an_vec2 wp = ani_affine_apply(w, (an_vec2){ xi, top_local });
    const float cr = cosf(-cam.rot), sr = sinf(-cam.rot);
    const float ry = sr * (wp.x - cam.pos.x) + cr * (wp.y - cam.pos.y);
    const float screen_top = (float)s->height * 0.5f - cam.shake.y + cam.zoom * ry;
    const float sy = sqrtf(fabsf(w.a * w.d - w.b * w.c)) * cam.zoom; // screen px per array unit
    const float hmax = sy > 0.0f ? (screen_top - 24.0f) / sy : H;
    if (H > hmax) {
        const float k = hmax > 0.0f ? hmax / H : 0.0f; // scale both arcs down together
        H *= k;
        HB *= k;
        if (H < hmin)
            fprintf(stderr, "anown: an_array_swap(%d, %d): arcs clamped to %.0f px by the frame (%.0f px needed); "
                            "cells may touch mid-swap\n", i, j, (double)(H * sy), (double)(hmin * sy));
    }
    // overlapping swaps on a slot: well-defined as tracks, rarely meant
    if (start < a->u.array.busy[i] || start < a->u.array.busy[j])
        fprintf(stderr, "anown: an_array_swap(%d, %d) overlaps a running swap on slot %d\n", i, j,
                start < a->u.array.busy[i] ? i : j);

    const double secs = x->opts.duration > 0.0f ? x->opts.duration : 0.9;
    const int64_t T = ani_ticks(secs);
    an_ease ease = x->opts.ease;
    if (ease.kind == AN_EASE_KIND_SPRING) ease = AN_EASE_IN_OUT;
    const int64_t endA = ani_record_lift(s, ani_obj_prop(A, AN_PROP_POS), start, T, ease, (ani_value){ { xj, 0 } }, H, lift);
    int64_t b1 = endA;
    if (adjacent) {
        // B slides while A is at least h up: A's eased progress in [lift, 1 - lift];
        // in-out ease: 4 t^3 = lift for t < 1/2
        const double t0 = ease.kind == AN_EASE_KIND_LINEAR ? lift : cbrt(lift / 4.0);
        const int64_t b0 = start + (int64_t)llround(t0 * (double)T);
        b1 = start + T - (int64_t)llround(t0 * (double)T);
        if (b1 > b0) ani_record(s, ani_obj_prop(B, AN_PROP_POS), b0, b1 - b0, ease, (ani_value){ { xi, 0 } }, false, NULL);
        else ani_record(s, ani_obj_prop(B, AN_PROP_POS), start + T / 2, 0, ease, (ani_value){ { xi, 0 } }, false, NULL);
    } else {
        ani_record_lift(s, ani_obj_prop(B, AN_PROP_POS), start, T, ease, (ani_value){ { xi, 0 } }, HB, lift);
    }
    // a small squash as each lands
    const an_anim sa = an_squash_stretch(A, &(an_verb_opts){ .amount = 0.12f });
    const an_anim sb = an_squash_stretch(B, &(an_verb_opts){ .amount = 0.12f });
    const int64_t ea_end = ani_record_anim(s, &sa, endA);
    ani_record_anim(s, &sb, b1);
    // script time: the slots swap owners
    a->u.array.perm[i] = eb;
    a->u.array.perm[j] = ea;
    a->u.array.busy[i] = a->u.array.busy[j] = endA;
    (void)ea_end;
    return endA;
}

// ---- named cell states ---------------------------------------------------------------------

an_anim an_array_set_state(an_obj *a, int slot, an_cell_state state, const an_anim_opts *opts) {
    an_anim x = { .obj = a, .verb = AN_VERB_CELL_STATE, .swap_i = slot, .swap_j = (int)state };
    if (opts) x.opts = *opts;
    if (!(x.opts.duration > 0.0f)) x.opts.duration = 0.3f;
    return x;
}

int64_t ani_array_record_state(an_scene *s, const an_anim *x, int64_t start) {
    an_obj *a = x->obj;
    const int slot = x->swap_i, st = x->swap_j;
    if (!valid(a, slot) || st < 0 || st > 3) return start;
    an_obj *cell = an_array_cell(a, slot);
    ani_record(s, ani_obj_prop(cell, AN_PROP_CELL_STATE), start, 0, AN_EASE_LINEAR, (ani_value){ { (double)st } },
               false, NULL);
    const an_anim f = an_fill_to(cell, a->u.array.states[st], &x->opts);
    return ani_record_anim(s, &f, start);
}

an_cell_state an_array_state_at_cursor(const an_obj *a, int slot) {
    if (!valid(a, slot)) return AN_CELL_BASE;
    an_obj *cell = an_array_cell(a, slot);
    const ani_value v = ani_sample(ani_obj_prop(cell, AN_PROP_CELL_STATE), ani_tick_time(a->scene->cursor));
    return (an_cell_state)(int)v.v[0];
}

// ---- script-time bounds --------------------------------------------------------------------

an_bounds an_array_slot_bounds(const an_obj *a, int slot) {
    if (!valid(a, slot)) return (an_bounds){ { 1, 1 }, { 0, 0 } };
    const ani_affine w = ani_obj_world_at((an_obj *)a, ani_tick_time(a->scene->cursor));
    const bool bars = a->u.array.style == AN_ARRAY_BARS;
    const float x = a->u.array.slot_x[slot], hw = a->u.array.cell.x * 0.5f;
    const float y0 = bars ? -a->u.array.cell.y : -a->u.array.cell.y * 0.5f, y1 = bars ? 0.0f : a->u.array.cell.y * 0.5f;
    an_bounds b = { { INFINITY, INFINITY }, { -INFINITY, -INFINITY } };
    const an_vec2 c[4] = { { x - hw, y0 }, { x + hw, y0 }, { x + hw, y1 }, { x - hw, y1 } };
    for (int k = 0; k < 4; k++) {
        const an_vec2 p = ani_affine_apply(w, c[k]);
        b.min.x = fminf(b.min.x, p.x); b.min.y = fminf(b.min.y, p.y);
        b.max.x = fmaxf(b.max.x, p.x); b.max.y = fmaxf(b.max.y, p.y);
    }
    return b;
}

float an_array_swap_clearance(const an_obj *a, int i, int j) {
    if (!valid(a, i) || !valid(a, j) || i == j) return 0.0f;
    float H, HB, h;
    double lift;
    swap_heights(a, i, j, &H, &HB, &lift, &h);
    const ani_affine w = ani_obj_world_at((an_obj *)a, ani_tick_time(a->scene->cursor));
    return H * sqrtf(fabsf(w.a * w.d - w.b * w.c)); // the row's top moves up by H (the arc's peak)
}
