// Axes and plots (docs/graphs.md). Everything is computed at script time: the
// function is sampled adaptively and clipped to the plot box as geometry
// (Liang-Barsky), so neither a layer nor a stencil bit is needed.
#include "scene_internal.h"
#include "tess_internal.h"
#include "timeline_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static float nice_step(float range) {
    if (!(range > 0.0f)) return 1.0f;
    const double raw = range / 6.0, p = pow(10.0, floor(log10(raw))), f = raw / p;
    return (float)((f < 1.5 ? 1.0 : f < 3.5 ? 2.0 : f < 7.5 ? 5.0 : 10.0) * p);
}

an_vec2 an_axes_local(const an_obj *ax, float x, float y) {
    if (!ax || ax->kind != AN_KIND_AXES) return (an_vec2){ 0, 0 };
    const float w = ax->u.axes.size.x, h = ax->u.axes.size.y;
    // multiply first: exact for round values (6 x 800 / 10 = 480, where 6 / 10 x 800 isn't)
    return (an_vec2){ (x - ax->u.axes.x_min) * w / (ax->u.axes.x_max - ax->u.axes.x_min),
                      h - (y - ax->u.axes.y_min) * h / (ax->u.axes.y_max - ax->u.axes.y_min) };
}

an_vec2 an_axes_point_at_cursor(const an_obj *ax, float x, float y) {
    if (!ax || ax->kind != AN_KIND_AXES) return (an_vec2){ 0, 0 };
    const ani_affine w = ani_obj_world_at((an_obj *)ax, ani_tick_time(ax->scene->cursor)); // sampling only reads
    return ani_affine_apply(w, an_axes_local(ax, x, y));
}

static an_obj *ink_line(an_scene *s, an_obj *parent, an_vec2 a, an_vec2 b, float width, an_color ink, bool undrawn,
                        const char *part, int index, float taper) {
    an_obj *l = an_stroke(s, &(an_stroke_opts){ .parent = parent, .stroke = ink, .stroke_width = width, .undrawn = undrawn,
                                                .segs = (an_seg[]){ AN_MOVE(a.x, a.y), AN_LINE(b.x, b.y) }, .seg_count = 2,
                                                .brush = { .taper_out = taper } });
    if (l) ani_obj_part(l, part, index);
    return l;
}

static an_obj *label(an_scene *s, an_obj *parent, const char *text, an_vec2 pos, float size, an_align al,
                     an_anchor an, an_color ink, bool undrawn, const char *part, int index) {
    an_obj *t = an_text(s, &(an_text_opts){ .parent = parent, .text = text, .pos = pos, .size = size, .align = al,
                                             .anchor = an, .fill = ink, .font = an_font_mono(s), .undrawn = undrawn });
    if (t) ani_obj_part(t, part, index);
    return t;
}

an_obj *an_axes(an_scene *s, const an_axes_opts *opts) {
    an_axes_opts o = opts ? *opts : (an_axes_opts){ 0 };
    if (!s) return NULL;
    if (!(o.size.x > 0 && o.size.y > 0)) o.size = (an_vec2){ 800, 500 };
    if (!(o.x_max > o.x_min)) { o.x_min = 0; o.x_max = 10; }
    if (!(o.y_max > o.y_min)) { o.y_min = 0; o.y_max = 10; }
    if (!(o.x_step > 0)) o.x_step = nice_step(o.x_max - o.x_min);
    if (!(o.y_step > 0)) o.y_step = nice_step(o.y_max - o.y_min);
    const float ls = o.label_size > 0 ? o.label_size : 26.0f;
    const an_color ink = o.stroke.a > 0 ? o.stroke : s->style.palette->ink;
    const float sw = o.stroke_width > 0 ? o.stroke_width : 5.0f;
    an_obj *ax = ani_obj_make(s, AN_KIND_AXES, &(ani_common_opts){ .name = o.name, .hidden = o.hidden, .parent = o.parent,
                                                                     .pos = o.pos, .scale = o.scale, .rotation = o.rotation,
                                                                     .z = o.z });
    if (!ax) return NULL;
    ax->u.axes.size = o.size;
    ax->u.axes.x_min = o.x_min;
    ax->u.axes.x_max = o.x_max;
    ax->u.axes.y_min = o.y_min;
    ax->u.axes.y_max = o.y_max;
    const float w = o.size.x, h = o.size.y;
    // the axes, a little past the box, tapering off like a pen lifting
    if (!ink_line(s, ax, (an_vec2){ -sw, h }, (an_vec2){ w + 24, h }, sw, ink, o.undrawn, "x axis", 0, 30) ||
        !ink_line(s, ax, (an_vec2){ 0, h + sw }, (an_vec2){ 0, -24 }, sw, ink, o.undrawn, "y axis", 0, 30))
        return NULL;
    char buf[32];
    const int x0 = (int)ceil(o.x_min / o.x_step - 1e-4), x1 = (int)floor(o.x_max / o.x_step + 1e-4);
    for (int k = x0; k <= x1; k++) {
        const float v = (float)((double)k * o.x_step), px = an_axes_local(ax, v, o.y_min).x;
        if (!ink_line(s, ax, (an_vec2){ px, h }, (an_vec2){ px, h + 12 }, sw * 0.6f, ink, o.undrawn, "x tick", k - x0, 0))
            return NULL;
        snprintf(buf, sizeof buf, "%g", (double)v);
        if (!label(s, ax, buf, (an_vec2){ px, h + 20 }, ls, AN_ALIGN_CENTER, AN_ANCHOR_TOP, ink, o.undrawn, "x tick label", k - x0))
            return NULL;
    }
    const int y0 = (int)ceil(o.y_min / o.y_step - 1e-4), y1 = (int)floor(o.y_max / o.y_step + 1e-4);
    for (int k = y0; k <= y1; k++) {
        const float v = (float)((double)k * o.y_step), py = an_axes_local(ax, o.x_min, v).y;
        if (!ink_line(s, ax, (an_vec2){ 0, py }, (an_vec2){ -12, py }, sw * 0.6f, ink, o.undrawn, "y tick", k - y0, 0))
            return NULL;
        snprintf(buf, sizeof buf, "%g", (double)v);
        if (!label(s, ax, buf, (an_vec2){ -20, py }, ls, AN_ALIGN_RIGHT, AN_ANCHOR_MIDDLE, ink, o.undrawn, "y tick label", k - y0))
            return NULL;
    }
    if (o.x_label && !label(s, ax, o.x_label, (an_vec2){ w, h + 20 + ls * 1.6f }, ls * 1.2f, AN_ALIGN_RIGHT, AN_ANCHOR_TOP,
                            ink, o.undrawn, "x label", 0))
        return NULL;
    if (o.y_label && !label(s, ax, o.y_label, (an_vec2){ 0, -40 }, ls * 1.2f, AN_ALIGN_LEFT, AN_ANCHOR_BOTTOM, ink,
                            o.undrawn, "y label", 0))
        return NULL;
    return ax;
}

// ---- function plots ------------------------------------------------------------------------

typedef struct samples {
    an_vec2 *p;
    bool *ok; // finite
    int n, cap;
} samples;

static bool push(samples *sm, an_vec2 p, bool ok) {
    if (sm->n == sm->cap) {
        const int cap = sm->cap ? sm->cap * 2 : 256;
        an_vec2 *np = realloc(sm->p, sizeof(an_vec2) * (size_t)cap);
        if (!np) return false;
        sm->p = np;
        bool *nk = realloc(sm->ok, sizeof(bool) * (size_t)cap);
        if (!nk) return false;
        sm->ok = nk;
        sm->cap = cap;
    }
    sm->p[sm->n] = p;
    sm->ok[sm->n++] = ok;
    return true;
}

typedef struct fn_ctx {
    const an_obj *ax;
    an_fn f;
    void *user;
} fn_ctx;

static bool eval(const fn_ctx *c, float x, an_vec2 *p) {
    const float y = c->f(x, c->user);
    if (!isfinite(y)) return false;
    *p = an_axes_local(c->ax, x, y);
    return isfinite(p->x) && isfinite(p->y);
}

enum { START = 32, DEPTH = 8 };
#define TOL 0.25f

// emit the samples after x0 up to and including x1 (both ends already evaluated)
static bool subdivide(const fn_ctx *c, samples *sm, float x0, an_vec2 p0, bool ok0, float x1, an_vec2 p1, bool ok1, int depth) {
    if (depth < DEPTH) {
        const float xm = x0 + (x1 - x0) * 0.5f;
        an_vec2 pm;
        const bool okm = eval(c, xm, &pm);
        bool split = !ok0 || !ok1 || !okm;
        if (!split) {
            const float dx = p1.x - p0.x, dy = p1.y - p0.y, len = sqrtf(dx * dx + dy * dy);
            const float dist = len > 0 ? fabsf((pm.x - p0.x) * dy - (pm.y - p0.y) * dx) / len
                                       : sqrtf((pm.x - p0.x) * (pm.x - p0.x) + (pm.y - p0.y) * (pm.y - p0.y));
            split = dist > TOL;
        }
        if (split && !(ok0 == okm && okm == ok1 && !ok0)) // all three bad: nothing to refine
            return subdivide(c, sm, x0, p0, ok0, xm, pm, okm, depth + 1) && subdivide(c, sm, xm, pm, okm, x1, p1, ok1, depth + 1);
    }
    return push(sm, p1, ok1);
}

// Liang-Barsky: clip a->b to [lo, hi]; t0, t1 the kept parameter range
static bool clip(an_vec2 a, an_vec2 b, an_vec2 lo, an_vec2 hi, float *t0, float *t1) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float p[4] = { -dx, dx, -dy, dy }, q[4] = { a.x - lo.x, hi.x - a.x, a.y - lo.y, hi.y - a.y };
    *t0 = 0.0f;
    *t1 = 1.0f;
    for (int k = 0; k < 4; k++) {
        if (p[k] == 0.0f) {
            if (q[k] < 0.0f) return false;
            continue;
        }
        const float r = q[k] / p[k];
        if (p[k] < 0.0f) {
            if (r > *t1) return false;
            if (r > *t0) *t0 = r;
        } else {
            if (r < *t0) return false;
            if (r < *t1) *t1 = r;
        }
    }
    return *t0 <= *t1;
}

static an_vec2 lerp(an_vec2 a, an_vec2 b, float t) { return (an_vec2){ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t }; }

an_obj *an_plot_function(an_obj *ax, an_fn f, void *user, const an_plot_opts *opts) {
    const an_plot_opts o = opts ? *opts : (an_plot_opts){ 0 };
    if (!ax || ax->kind != AN_KIND_AXES || !f) return NULL;
    an_scene *s = ax->scene;
    const float from = (o.from != 0 || o.to != 0) ? o.from : ax->u.axes.x_min;
    const float to = (o.from != 0 || o.to != 0) ? o.to : ax->u.axes.x_max;
    if (!(to > from)) return NULL;
    const fn_ctx c = { ax, f, user };
    samples sm = { 0 };
    an_vec2 p0;
    bool ok0 = eval(&c, from, &p0);
    bool ok = push(&sm, p0, ok0);
    for (int k = 0; ok && k < START; k++) {
        const float x0 = from + (to - from) * (float)k / START, x1 = from + (to - from) * (float)(k + 1) / START;
        an_vec2 p1;
        const bool ok1 = eval(&c, x1, &p1);
        ok = subdivide(&c, &sm, x0, p0, ok0, x1, p1, ok1, 0);
        p0 = p1;
        ok0 = ok1;
    }
    // clip to the box, inset so the ink stays inside even while it boils
    const float sw = o.stroke_width > 0 ? o.stroke_width : 5.0f;
    const float inset = sw * 0.5f + s->style.boil;
    const an_vec2 lo = { inset, inset }, hi = { ax->u.axes.size.x - inset, ax->u.axes.size.y - inset };
    ani_path_builder b;
    ani_pb_init(&b);
    bool pen = false; // a subpath is open and ends at the last kept point
    for (int i = 0; ok && i + 1 < sm.n; i++) {
        if (!sm.ok[i] || !sm.ok[i + 1]) {
            pen = false;
            continue;
        }
        float t0, t1;
        if (!clip(sm.p[i], sm.p[i + 1], lo, hi, &t0, &t1)) {
            pen = false;
            continue;
        }
        if (!pen || t0 > 0.0f) ok = ani_pb_move(&b, lerp(sm.p[i], sm.p[i + 1], t0));
        ok = ok && ani_pb_line(&b, t1 < 1.0f ? lerp(sm.p[i], sm.p[i + 1], t1) : sm.p[i + 1]);
        pen = t1 >= 1.0f;
    }
    free(sm.p);
    free(sm.ok);
    ani_path *path = ok ? ani_pb_finish(&b, &s->arena) : NULL;
    ani_pb_free(&b);
    if (!path || path->npts == 0) {
        if (ok) fprintf(stderr, "anown: an_plot_function: nothing of the curve is inside the axes\n");
        return NULL;
    }
    ani_common_opts cm = *ANI_COMMON(&o);
    cm.parent = ax;
    cm.pos = (an_vec2){ 0, 0 };
    cm.stroke = o.stroke.a > 0 ? o.stroke : s->style.palette->b;
    cm.stroke_width = sw;
    return ani_obj_with_path(ani_obj_make(s, AN_KIND_STROKE, &cm), path, true);
}

// ---- data plots ---------------------------------------------------------------------------

an_obj *an_plot_data(an_obj *ax, const float *xs, const float *ys, int n, const an_plot_opts *opts) {
    const an_plot_opts o = opts ? *opts : (an_plot_opts){ 0 };
    if (!ax || ax->kind != AN_KIND_AXES || !xs || !ys || n <= 0) return NULL;
    an_scene *s = ax->scene;
    an_obj *g = an_group(s, &(an_group_opts){ .parent = ax, .name = o.name, .hidden = o.hidden, .z = o.z });
    if (!g) return NULL;
    const an_color fill = o.fill.a > 0 ? o.fill : s->style.palette->a;
    const an_color ink = o.stroke.a > 0 ? o.stroke : s->style.palette->ink;
    const float sw = o.stroke_width > 0 ? o.stroke_width : 4.0f;
    const float ylo = ax->u.axes.y_min, yhi = ax->u.axes.y_max;
    const float base = 0.0f < ylo ? ylo : 0.0f > yhi ? yhi : 0.0f;
    int skipped = 0;
    for (int k = 0; k < n; k++) {
        const bool xin = xs[k] >= ax->u.axes.x_min && xs[k] <= ax->u.axes.x_max;
        if (o.style == AN_PLOT_BARS) {
            if (!xin) {
                skipped++;
                continue;
            }
            const float v = ys[k] < ylo ? ylo : ys[k] > yhi ? yhi : ys[k]; // clipped to the box
            const an_vec2 top = an_axes_local(ax, xs[k], v), bot = an_axes_local(ax, xs[k], base);
            const float bw = o.size > 0 ? o.size : 0.6f * ax->u.axes.size.x / (float)n;
            const float bh = fabsf(bot.y - top.y);
            an_obj *r = an_rect(s, &(an_rect_opts){ .parent = g, .pos = { top.x, (top.y + bot.y) * 0.5f },
                                                    .size = { bw, bh > 0 ? bh : 0.001f }, .fill = fill, .stroke = ink,
                                                    .stroke_width = sw, .pattern = o.pattern, .undrawn = o.undrawn,
                                                    .riso = o.riso, .shadow = o.shadow, .brush = o.brush,
                                                    .paper_under = o.paper_under });
            if (!r) return NULL;
            ani_obj_part(r, "bar", k);
        } else {
            if (!xin || ys[k] < ylo || ys[k] > yhi) {
                skipped++;
                continue;
            }
            an_obj *pt = an_circle(s, &(an_circle_opts){ .parent = g, .pos = an_axes_local(ax, xs[k], ys[k]),
                                                         .radius = o.size > 0 ? o.size : 6.0f, .fill = fill, .stroke = ink,
                                                         .stroke_width = sw, .undrawn = o.undrawn, .pattern = o.pattern,
                                                         .riso = o.riso, .shadow = o.shadow, .brush = o.brush,
                                                         .paper_under = o.paper_under });
            if (!pt) return NULL;
            ani_obj_part(pt, "point", k);
        }
    }
    if (skipped) fprintf(stderr, "anown: an_plot_data: %d point(s) outside the axes' range skipped\n", skipped);
    return g;
}

an_obj *an_plot_item(const an_obj *plot, int k) {
    if (!plot || k < 0) return NULL;
    an_obj *c = plot->first_child;
    for (int i = 0; c && i < k; i++) c = c->next_sibling;
    return c;
}
