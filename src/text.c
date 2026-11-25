// Text and code blocks: layout once at creation, one object per letter.
// docs/text.md is the spec.
#include "text_internal.h"
#include "font_internal.h"
#include "scene_internal.h"
#include "timeline_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- UTF-8 -------------------------------------------------------------------------

// Next codepoint of s (advancing *i); invalid bytes decode as U+FFFD.
static uint32_t utf8_next(const unsigned char *s, size_t *i) {
    const unsigned char c = s[*i];
    int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
    if (n < 0) {
        (*i)++;
        return 0xFFFD;
    }
    uint32_t cp = n == 0 ? c : (uint32_t)(c & (0x3F >> n));
    for (int k = 1; k <= n; k++) {
        const unsigned char d = s[*i + (size_t)k];
        if ((d >> 6) != 2) {
            *i += (size_t)k;
            return 0xFFFD;
        }
        cp = (cp << 6) | (d & 0x3F);
    }
    *i += (size_t)n + 1;
    return cp;
}

// ---- layout ---------------------------------------------------------------------------

typedef struct lay_glyph {
    int gi;
    float x;  // pen x within its line (before alignment), px
    int line;
} lay_glyph;

typedef struct layout {
    lay_glyph *g;
    int n, cap;
    float *line_w; // pen x at each line's end (advance width), px
    int nlines, line_cap;
} layout;

static bool lay_push(layout *L, lay_glyph g) {
    if (L->n == L->cap) {
        const int c = L->cap ? L->cap * 2 : 64;
        lay_glyph *p = realloc(L->g, sizeof *p * (size_t)c);
        if (!p) return false;
        L->g = p;
        L->cap = c;
    }
    L->g[L->n++] = g;
    return true;
}

static bool lay_line(layout *L, float w) {
    if (L->nlines == L->line_cap) {
        const int c = L->line_cap ? L->line_cap * 2 : 16;
        float *p = realloc(L->line_w, sizeof *p * (size_t)c);
        if (!p) return false;
        L->line_w = p;
        L->line_cap = c;
    }
    L->line_w[L->nlines++] = w;
    return true;
}

// Pen positions of every glyph (spaces included), line by line. k: px per unit.
static bool layout_run(layout *L, an_font *f, const char *str, float k, bool kerning) {
    const unsigned char *s = (const unsigned char *)str;
    size_t i = 0;
    float x = 0.0f;
    int prev = -1;
    while (s[i]) {
        uint32_t cp = utf8_next(s, &i);
        if (cp == '\r') continue;
        if (cp == '\n') {
            if (!lay_line(L, x)) return false;
            x = 0.0f;
            prev = -1;
            continue;
        }
        int reps = 1;
        if (cp == '\t') {
            cp = ' ';
            reps = 4;
        }
        const int gi = ani_font_index(f, cp);
        const ani_glyph *g = ani_font_glyph(f, gi);
        if (!g) return false;
        for (int r = 0; r < reps; r++) {
            if (prev >= 0 && kerning) x += (float)ani_font_kern(f, prev, gi) * k;
            if (!lay_push(L, (lay_glyph){ .gi = gi, .x = x, .line = L->nlines })) return false;
            x += (float)g->advance * k;
            prev = gi;
        }
    }
    return lay_line(L, x);
}

// The glyph's outline scaled to the size, with the pivot at the origin.
static ani_path *glyph_path(ani_arena *a, const ani_path *src, float k, an_vec2 pivot) {
    ani_path_builder b;
    ani_pb_init(&b);
    bool ok = true;
    for (int c = 0; ok && c < src->nsubs; c++) {
        const ani_subpath *sp = &src->subs[c];
        for (int i = 0; ok && i < sp->count; i++) {
            const an_vec2 p = src->pts[sp->first + i];
            const an_vec2 q = { p.x * k - pivot.x, p.y * k - pivot.y };
            ok = i == 0 ? ani_pb_move(&b, q) : ani_pb_line(&b, q);
        }
        if (sp->closed) ani_pb_close(&b);
    }
    ani_path *p = ok ? ani_pb_finish(&b, a) : NULL;
    ani_pb_free(&b);
    return p;
}

typedef struct text_style {
    an_color fill, stroke;
    float stroke_width;
    bool outline, no_fill;
    an_brush brush;
    an_toggle riso, shadow;
    an_pattern pattern;
    an_toggle paper_under;
    bool hidden, undrawn;
} text_style;

typedef struct text_params {
    an_font *font;
    float size;
    an_align align;
    an_anchor anchor;
    float line_spacing;
    bool kerning, size_chaos;
    float line_pitch; // > 0: exact baseline-to-baseline distance (code blocks)
} text_params;

// The letters of one string, made under `holder` (the text itself, or one of its
// versions) and laid out for text `t`.
typedef struct letters {
    an_obj **glyphs;
    int n;
    float total_arc;
    int *seq, *seq_letter, nseq; // glyph ids in order, -1 per line break; letter index or -1
    an_vec2 vpivot;              // the block's center
} letters;

static bool make_letters(an_scene *s, an_obj *t, an_obj *holder, const char *copy, const text_params *tp,
                         const text_style *ts, letters *out) {
    an_font *f = tp->font;
    const float k = tp->size / (float)f->upem;
    layout L = { 0 };
    if (!layout_run(&L, f, copy, k, tp->kerning)) {
        free(L.g);
        free(L.line_w);
        return false;
    }
    int ng = 0;
    for (int i = 0; i < L.n; i++) ng += ani_font_glyph(f, L.g[i].gi)->path != NULL;
    an_obj **glyphs = ani_arena_alloc(&s->arena, sizeof(an_obj *) * (size_t)(ng ? ng : 1), alignof(an_obj *));
    const int nseq = L.n + L.nlines - 1;
    int *seq = ani_arena_alloc(&s->arena, sizeof(int) * (size_t)(nseq > 0 ? nseq : 1), alignof(int));
    int *seq_letter = ani_arena_alloc(&s->arena, sizeof(int) * (size_t)(nseq > 0 ? nseq : 1), alignof(int));
    if (!glyphs || !seq || !seq_letter) {
        free(L.g);
        free(L.line_w);
        return false;
    }

    // vertical: baseline of line i = i * pitch + the anchor offset
    const float pitch = tp->line_pitch > 0.0f
        ? tp->line_pitch
        : (float)(f->ascent - f->descent + f->line_gap) * k * (tp->line_spacing > 0.0f ? tp->line_spacing : 1.0f);
    const float asc = (float)f->ascent * k, desc = (float)f->descent * k; // desc < 0
    const float last = (float)(L.nlines - 1) * pitch;
    float y0 = 0.0f;
    switch (tp->anchor) {
    case AN_ANCHOR_BASELINE: y0 = 0.0f; break;
    case AN_ANCHOR_TOP: y0 = asc; break;
    case AN_ANCHOR_BOTTOM: y0 = -(last - desc); break;
    case AN_ANCHOR_MIDDLE: y0 = -((-asc) + (last - desc)) * 0.5f; break;
    }
    const float xh = (float)f->x_height * k;
    float arc = 0.0f;
    int gi_out = 0, ns = 0, line = 0;
    for (int i = 0; i < L.n; i++) {
        for (; line < L.g[i].line; line++) { // a break before this glyph's line
            seq[ns] = -1;
            seq_letter[ns++] = -1;
        }
        const ani_glyph *g = ani_font_glyph(f, L.g[i].gi);
        seq[ns] = L.g[i].gi;
        seq_letter[ns++] = g->path ? gi_out : -1;
        if (!g->path) continue; // a space: no letter object
        const float lw = L.line_w[L.g[i].line];
        const float ax = tp->align == AN_ALIGN_CENTER ? -lw * 0.5f : tp->align == AN_ALIGN_RIGHT ? -lw : 0.0f;
        const an_vec2 pen = { L.g[i].x + ax, y0 + (float)L.g[i].line * pitch };
        const an_vec2 pivot = { (float)g->advance * k * 0.5f, -xh * 0.5f };
        ani_path *path = glyph_path(&s->arena, g->path, k, pivot);
        an_obj *o = path ? ani_obj_make(s, AN_KIND_GLYPH, &(ani_common_opts){
                               .parent = holder, .pos = { pen.x + pivot.x, pen.y + pivot.y }, .hidden = ts->hidden,
                               .undrawn = ts->undrawn, .fill = ts->fill, .stroke = ts->stroke,
                               .stroke_width = ts->stroke_width, .no_fill = ts->no_fill, .brush = ts->brush,
                               .riso = ts->riso, .shadow = ts->shadow, .pattern = ts->pattern,
                               .paper_under = ts->paper_under })
                         : NULL;
        if (!o) {
            free(L.g);
            free(L.line_w);
            return false;
        }
        o->path = path;
        o->has_stroke = ts->outline;
        o->u.glyph.glyph = g;
        o->u.glyph.text = t;
        o->u.glyph.index = gi_out;
        o->u.glyph.pen = pen;
        o->u.glyph.pivot = pivot;
        o->u.glyph.arc0 = arc;
        o->u.glyph.stem_px = (float)f->stem * k;
        o->u.glyph.em_px = tp->size;
        arc += path->total;
        glyphs[gi_out++] = o;
    }
    for (; line < L.nlines - 1; line++) { // trailing breaks
        seq[ns] = -1;
        seq_letter[ns++] = -1;
    }
    // the verb pivot: the center of the block (advance widths x ascent to descent)
    float bx0 = 0.0f, bx1 = 0.0f;
    for (int l = 0; l < L.nlines; l++) {
        const float lw = L.line_w[l];
        const float ax = tp->align == AN_ALIGN_CENTER ? -lw * 0.5f : tp->align == AN_ALIGN_RIGHT ? -lw : 0.0f;
        if (l == 0 || ax < bx0) bx0 = ax;
        if (l == 0 || ax + lw > bx1) bx1 = ax + lw;
    }
    *out = (letters){ .glyphs = glyphs, .n = gi_out, .total_arc = arc, .seq = seq, .seq_letter = seq_letter,
                      .nseq = ns, .vpivot = { (bx0 + bx1) * 0.5f, ((y0 - asc) + (y0 + last - desc)) * 0.5f } };
    free(L.g);
    free(L.line_w);
    return true;
}

static an_obj *make_text(an_scene *s, const ani_common_opts *node, const char *str, const text_params *tp,
                         const text_style *ts) {
    an_font *f = tp->font;
    const char *copy = ani_arena_strdup(&s->arena, str);
    if (!f || !copy) return NULL;
    // the node carries the letters' paint too: letters follow its fill, stroke
    // and stroke width until overridden (docs/text.md)
    ani_common_opts nopts = *node;
    nopts.fill = ts->fill;
    nopts.stroke = ts->stroke;
    nopts.stroke_width = ts->stroke_width;
    an_obj *t = ani_obj_make(s, AN_KIND_TEXT, &nopts);
    if (!t) return NULL;
    t->u.text.str = copy;
    t->u.text.size = tp->size;
    t->u.text.font = f;
    t->u.text.size_chaos = tp->size_chaos;
    t->u.text.inked = ts->outline;
    // kept for an_text_set: the same layout and paint for every version
    text_params *tpc = ani_arena_alloc(&s->arena, sizeof *tpc, alignof(text_params));
    text_style *tsc = ani_arena_alloc(&s->arena, sizeof *tsc, alignof(text_style));
    if (!tpc || !tsc) return NULL;
    *tpc = *tp;
    *tsc = *ts;
    tsc->hidden = tsc->undrawn = false; // later versions appear by their own fades
    t->u.text.params = tpc;
    t->u.text.style = tsc;
    letters L;
    if (!make_letters(s, t, t, copy, tp, ts, &L)) return NULL;
    t->vpivot = L.vpivot;
    t->u.text.glyphs = L.glyphs;
    t->u.text.nglyphs = L.n;
    t->u.text.total_arc = L.total_arc;
    t->u.text.seq = L.seq;
    t->u.text.seq_letter = L.seq_letter;
    t->u.text.nseq = L.nseq;
    return t;
}

an_obj *an_text(an_scene *s, const an_text_opts *opts) {
    const an_text_opts o = opts ? *opts : (an_text_opts){ 0 };
    if (!s) return NULL;
    an_font *f = o.font ? o.font : an_font_display(s);
    if (!f || (o.font && o.font->scene != s)) return NULL;
    const text_params tp = {
        .font = f, .size = o.size > 0.0f ? o.size : 48.0f, .align = o.align, .anchor = o.anchor,
        .line_spacing = o.line_spacing, .kerning = o.kerning != AN_OFF, .size_chaos = o.size_chaos != AN_OFF,
    };
    // the node takes the transform, name, z, visibility and draw progress; the
    // letters take the paint style
    ani_common_opts node = { .name = o.name, .hidden = o.hidden, .undrawn = o.undrawn, .parent = o.parent,
                             .pos = o.pos, .scale = o.scale, .rotation = o.rotation, .z = o.z };
    const text_style ts = {
        .fill = o.fill, .stroke = o.stroke, .stroke_width = o.stroke_width,
        .outline = ani_outline_requested(o.stroke, o.stroke_width), .no_fill = o.no_fill, .brush = o.brush,
        .riso = o.riso, .shadow = o.shadow, .pattern = o.pattern, .paper_under = o.paper_under,
        .hidden = o.letters_hidden, .undrawn = o.letters_undrawn,
    };
    return make_text(s, &node, o.text ? o.text : "", &tp, &ts);
}

// ---- versions (an_text_set, docs/api-fixes.md item 2) --------------------------------------

enum { MAX_VERSIONS = 256 };

an_obj *ani_text_current(const an_obj *t, ani_time at) {
    if (!t || t->kind != AN_KIND_TEXT || t->u.text.nver == 0) return NULL;
    for (int v = t->u.text.nver - 1; v > 0; v--) {
        const an_obj *x = t->u.text.versions[v];
        if (x->u.tver.start >= 0 && ani_reached(x->u.tver.start, at)) return t->u.text.versions[v];
    }
    return t->u.text.versions[0];
}

bool ani_text_version_live(const an_obj *v, ani_time at) {
    if (v->u.tver.start < 0) return false; // never played: never seen
    return v->u.tver.live_to == INT64_MAX || !ani_reached(v->u.tver.live_to + 1, at);
}

int an_text_length(const an_obj *t) {
    if (!t || t->kind != AN_KIND_TEXT) return 0;
    const an_obj *v = ani_text_current(t, ani_tick_time(t->scene->cursor));
    return v ? v->u.tver.n : t->u.text.nglyphs;
}

an_obj *an_text_glyph(const an_obj *t, int i) {
    if (!t || t->kind != AN_KIND_TEXT || i < 0) return NULL;
    const an_obj *v = ani_text_current(t, ani_tick_time(t->scene->cursor));
    if (v) return i < v->u.tver.n ? v->u.tver.glyphs[i] : NULL;
    return i < t->u.text.nglyphs ? t->u.text.glyphs[i] : NULL;
}

static bool push_version(an_obj *t, an_obj *v) {
    if (t->u.text.nver == t->u.text.vcap) {
        const int cap = t->u.text.vcap ? t->u.text.vcap * 2 : 4;
        an_obj **p = ani_arena_alloc(&t->scene->arena, sizeof(an_obj *) * (size_t)cap, alignof(an_obj *));
        if (!p) return false;
        if (t->u.text.nver) memcpy(p, t->u.text.versions, sizeof(an_obj *) * (size_t)t->u.text.nver);
        t->u.text.versions = p;
        t->u.text.vcap = cap;
    }
    t->u.text.versions[t->u.text.nver++] = v;
    return true;
}

// The first an_text_set: the creation letters move into version 0 (an identity
// node born with the text). Reparenting keeps their keys, so nothing changes.
static bool make_version0(an_obj *t) {
    an_obj *v = ani_obj_make(t->scene, AN_KIND_TEXT_VERSION, &(ani_common_opts){ .parent = t });
    if (!v) return false;
    ani_obj_part(v, "version", 0);
    v->birth = t->birth;
    for (int i = 0; i < t->u.text.nglyphs; i++)
        if (t->u.text.glyphs[i]->parent == t) an_obj_set_parent(t->u.text.glyphs[i], v);
    v->u.tver.text = t;
    v->u.tver.glyphs = t->u.text.glyphs;
    v->u.tver.n = t->u.text.nglyphs;
    v->u.tver.total_arc = t->u.text.total_arc;
    v->u.tver.seq = t->u.text.seq;
    v->u.tver.seq_letter = t->u.text.seq_letter;
    v->u.tver.nseq = t->u.text.nseq;
    v->u.tver.start = 0;
    v->u.tver.live_to = INT64_MAX;
    return push_version(t, v);
}

// LCS of a (old) and b (new); mb[j] = the a index matched to b[j], or -1. Read
// back from the end: equal -> match (diagonal); else up (drop a's) when
// L[i-1][j] >= L[i][j-1], else left. Ties drop the old letter, so repeats match
// their latest copies.
static bool lcs(const int *a, int n, const int *b, int m, int *mb) {
    int *L = calloc((size_t)(n + 1) * (size_t)(m + 1), sizeof(int));
    if (!L) return false;
#define LCS(i, j) L[(size_t)(i) * (size_t)(m + 1) + (size_t)(j)]
    for (int i = 1; i <= n; i++)
        for (int j = 1; j <= m; j++)
            LCS(i, j) = a[i - 1] == b[j - 1] ? LCS(i - 1, j - 1) + 1
                        : LCS(i - 1, j) >= LCS(i, j - 1) ? LCS(i - 1, j) : LCS(i, j - 1);
    for (int j = 0; j < m; j++) mb[j] = -1;
    int i = n, j = m;
    while (i > 0 && j > 0) {
        if (a[i - 1] == b[j - 1]) {
            mb[j - 1] = i - 1;
            i--;
            j--;
        } else if (LCS(i - 1, j) >= LCS(i, j - 1)) i--;
        else j--;
    }
#undef LCS
    free(L);
    return true;
}

an_anim an_text_set(an_obj *t, const char *str, const an_text_set_opts *opts) {
    const an_text_set_opts o = opts ? *opts : (an_text_set_opts){ 0 };
    an_anim x = { .verb = AN_VERB_TEXT_SET };
    if (!t || t->kind != AN_KIND_TEXT || !t->u.text.params) {
        fprintf(stderr, "anown: an_text_set needs a text (an_text)\n");
        x.failed = true;
        x.scene = t ? t->scene : NULL;
        return x;
    }
    an_scene *s = t->scene;
    x.scene = s;
    const int have = t->u.text.nver ? t->u.text.nver : 1;
    if (have >= MAX_VERSIONS) {
        fprintf(stderr, "anown: an_text_set on text \"%s\": %d versions already (the limit)\n",
                t->name ? t->name : t->u.text.str, MAX_VERSIONS);
        x.failed = true;
        return x;
    }
    if (t->u.text.nver == 0 && !make_version0(t)) {
        x.failed = true;
        return x;
    }
    const char *copy = ani_arena_strdup(&s->arena, str ? str : "");
    an_obj *v = copy ? ani_obj_make(s, AN_KIND_TEXT_VERSION, &(ani_common_opts){
                           .parent = t, .hidden = o.mode == AN_TEXT_CROSSFADE })
                     : NULL;
    if (!v) {
        x.failed = true;
        return x;
    }
    ani_obj_part(v, "version", t->u.text.nver);
    text_style ts = *(const text_style *)t->u.text.style;
    ts.hidden = o.mode == AN_TEXT_LETTERS; // LETTERS: each letter appears by its own track
    letters L;
    if (!make_letters(s, t, v, copy, (const text_params *)t->u.text.params, &ts, &L)) {
        x.failed = true;
        return x;
    }
    const an_obj *prev = t->u.text.versions[t->u.text.nver - 1];
    v->u.tver.text = t;
    v->u.tver.glyphs = L.glyphs;
    v->u.tver.n = L.n;
    v->u.tver.total_arc = L.total_arc;
    v->u.tver.seq = L.seq;
    v->u.tver.seq_letter = L.seq_letter;
    v->u.tver.nseq = L.nseq;
    v->u.tver.start = -1;
    v->u.tver.live_to = INT64_MAX;
    v->u.tver.mode = o.mode;
    v->u.tver.stagger = o.stagger < 0 ? 0 : o.stagger > 1 ? 1 : o.stagger;
    v->u.tver.gap = o.gap > 0 ? o.gap : 0.03f;
    v->u.tver.match = NULL;
    if (o.mode == AN_TEXT_LETTERS) {
        int *mb = malloc(sizeof(int) * (size_t)(L.nseq > 0 ? L.nseq : 1));
        int *match = ani_arena_alloc(&s->arena, sizeof(int) * (size_t)(L.n > 0 ? L.n : 1), alignof(int));
        if (!mb || !match || !lcs(prev->u.tver.seq, prev->u.tver.nseq, L.seq, L.nseq, mb)) {
            free(mb);
            x.failed = true;
            return x;
        }
        for (int k = 0; k < L.n; k++) match[k] = -1;
        for (int j = 0; j < L.nseq; j++) {
            const int nl = L.seq_letter[j], ol = mb[j] >= 0 ? prev->u.tver.seq_letter[mb[j]] : -1;
            if (nl < 0 || ol < 0) continue;
            match[nl] = ol;
            // the same letter going on: the same random stream, so the swap is invisible
            L.glyphs[nl]->rng_key = prev->u.tver.glyphs[ol]->rng_key;
            L.glyphs[nl]->rng = prev->u.tver.glyphs[ol]->rng;
        }
        free(mb);
        v->u.tver.match = match;
    }
    if (!push_version(t, v)) {
        x.failed = true;
        return x;
    }
    x.obj = t;
    x.swap_i = t->u.text.nver - 1;
    x.opts = o.anim;
    if (!(x.opts.duration > 0.0f)) x.opts.duration = 0.3f;
    return x;
}

int64_t ani_text_record_set(an_scene *s, const an_anim *a, int64_t start) {
    an_obj *t = a->obj;
    if (a->swap_i <= 0 || a->swap_i >= t->u.text.nver) return start;
    an_obj *v = t->u.text.versions[a->swap_i], *prev = t->u.text.versions[a->swap_i - 1];
    if (v->u.tver.start >= 0) {
        fprintf(stderr, "anown: an_text_set: this change was already played\n");
        return start;
    }
    v->u.tver.start = start;
    an_anim_opts op = a->opts;
    int64_t end = start;
    if (v->u.tver.mode == AN_TEXT_CROSSFADE) {
        const float f = op.duration / (1.0f + v->u.tver.stagger);
        op.duration = f;
        const an_anim out = an_fade_to(prev, 0.0f, &op), in = an_fade_to(v, 1.0f, &op);
        const int64_t e1 = ani_record_anim(s, &out, start);
        const int64_t e2 = ani_record_anim(s, &in, start + ani_ticks(v->u.tver.stagger * f));
        prev->u.tver.live_to = e1;
        return e1 > e2 ? e1 : e2;
    }
    // LETTERS
    const int64_t gap = ani_ticks(v->u.tver.gap);
    bool *kept = calloc((size_t)(prev->u.tver.n > 0 ? prev->u.tver.n : 1), sizeof(bool));
    if (!kept) return start;
    for (int k = 0; k < v->u.tver.n; k++)
        if (v->u.tver.match[k] >= 0) kept[v->u.tver.match[k]] = true;
    int64_t old_end = start;
    for (int k = 0, i = 0; k < prev->u.tver.n; k++) {
        an_obj *g = prev->u.tver.glyphs[k];
        if (kept[k]) { // hands over to its continuation, in place, at the start
            ani_record(s, ani_obj_prop(g, AN_PROP_OPACITY), start, 0, AN_EASE_LINEAR, (ani_value){ { 0.0 } }, false, NULL);
            continue;
        }
        const an_anim f = an_fade_to(g, 0.0f, &op);
        const int64_t e = ani_record_anim(s, &f, start + (int64_t)(i++) * gap);
        old_end = e > old_end ? e : old_end;
    }
    free(kept);
    for (int k = 0, j = 0; k < v->u.tver.n; k++) {
        an_obj *g = v->u.tver.glyphs[k];
        const int m = v->u.tver.match[k];
        if (m >= 0) {
            ani_record(s, ani_obj_prop(g, AN_PROP_OPACITY), start, 0, AN_EASE_LINEAR, (ani_value){ { 1.0 } }, false, NULL);
            const ani_value from = ani_sample(ani_obj_prop(prev->u.tver.glyphs[m], AN_PROP_POS), ani_tick_time(start));
            an_anim mv = an_move_to(g, g->pos, &op);
            mv.has_from = true;
            mv.from[0] = (float)from.v[0];
            mv.from[1] = (float)from.v[1];
            const int64_t e = ani_record_anim(s, &mv, start);
            end = e > end ? e : end;
            continue;
        }
        const an_anim f = an_fade_to(g, 1.0f, &op);
        const int64_t e = ani_record_anim(s, &f, start + (int64_t)(j++) * gap);
        end = e > end ? e : end;
    }
    prev->u.tver.live_to = old_end;
    return end > old_end ? end : old_end;
}

// ---- size factor and snapping (used by the tessellator and an_obj_chaos) --------------

float ani_text_size_factor(const an_obj *g, float wscale, float boil) {
    if (g->kind != AN_KIND_GLYPH || !g->u.glyph.text->u.text.size_chaos) return 1.0f;
    const float stem = g->u.glyph.stem_px * wscale;
    float ramp = (stem - 3.0f) / 5.0f;
    ramp = ramp < 0.0f ? 0.0f : ramp > 1.0f ? 1.0f : ramp;
    if (boil > 0.0f) {
        const float cap = 0.3f * stem / (2.0f * boil);
        if (cap < ramp) ramp = cap;
    }
    return ramp;
}

float ani_text_riso_scale(const an_obj *g, float wscale) {
    const float em = g->u.glyph.em_px * wscale / 100.0f;
    return em < 0.5f ? 0.5f : em > 3.0f ? 3.0f : em;
}

bool ani_text_snap(ani_affine pw, ani_affine *w, an_vec2 pen, float rotation, an_vec2 scale) {
    // only at an effective scale of exactly 1: the text's world transform is a
    // pure translation and the letter isn't rotated or scaled (docs/camera.md, (b))
    const float a = pw.a;
    if (a != 1.0f || pw.b != 0.0f || pw.c != 0.0f || pw.d != 1.0f) return false;
    if (rotation != 0.0f || scale.x != 1.0f || scale.y != 1.0f) return false;
    const float sx = a * pen.x, sy = a * pen.y;
    w->tx += roundf(sx) - sx;
    w->ty += roundf(sy) - sy;
    return true;
}

// ---- code blocks ------------------------------------------------------------------------

static an_color mix(an_color a, an_color b, float t) {
    return (an_color){ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1.0f };
}

an_obj *an_code(an_scene *s, const an_code_opts *opts) {
    const an_code_opts o = opts ? *opts : (an_code_opts){ 0 };
    if (!s) return NULL;
    an_font *f = o.font ? o.font : an_font_mono(s);
    if (!f || (o.font && o.font->scene != s)) return NULL;
    const an_palette *pal = s->style.palette;
    const float size = o.size > 0.0f ? o.size : 30.0f;
    const float k = size / (float)f->upem;
    const int first = o.first_line > 0 ? o.first_line : 1;
    // strip `//@name` anchors (and the spaces before them) from the display;
    // remember name -> line
    const char *raw = o.code ? o.code : "";
    char *code = ani_arena_alloc(&s->arena, strlen(raw) + 1, 1);
    if (!code) return NULL;
    const char *anames[256];
    int alines[256], nanch = 0, lineno = first;
    {
        char *out = code;
        const char *p = raw;
        while (*p) {
            const char *e = strchr(p, '\n');
            const size_t len = e ? (size_t)(e - p) : strlen(p);
            size_t keep = len;
            const char *m = NULL;
            for (size_t k = 0; k + 3 <= len; k++) {
                if (p[k] == '/' && p[k + 1] == '/' && p[k + 2] == '@') m = p + k; // the last one wins
            }
            if (m) {
                const char *n0 = m + 3, *n1 = n0;
                while (n1 < p + len && ((*n1 >= 'a' && *n1 <= 'z') || (*n1 >= 'A' && *n1 <= 'Z') ||
                                        (*n1 >= '0' && *n1 <= '9') || *n1 == '_' || *n1 == '-'))
                    n1++;
                bool trailing = true;
                for (const char *q = n1; q < p + len; q++) trailing = trailing && (*q == ' ' || *q == '\t' || *q == '\r');
                if (n1 > n0 && trailing && nanch < 256) {
                    char *name = ani_arena_alloc(&s->arena, (size_t)(n1 - n0) + 1, 1);
                    if (!name) return NULL;
                    memcpy(name, n0, (size_t)(n1 - n0));
                    anames[nanch] = name;
                    alines[nanch++] = lineno;
                    keep = (size_t)(m - p);
                    while (keep > 0 && (p[keep - 1] == ' ' || p[keep - 1] == '\t')) keep--;
                }
            }
            memcpy(out, p, keep);
            out += keep;
            if (e) *out++ = '\n';
            p = e ? e + 1 : p + len;
            lineno++;
        }
        *out = 0;
    }

    // split into lines (copies, so each line is its own text)
    int nlines = 1;
    for (const char *p = code; *p; p++) nlines += *p == '\n';
    an_obj *g = ani_obj_make(s, AN_KIND_CODE, &(ani_common_opts){
        .name = o.name, .hidden = o.hidden, .parent = o.parent, .pos = o.pos, .scale = o.scale,
        .rotation = o.rotation, .z = o.z });
    if (!g) return NULL;
    g->u.code.bars = ani_arena_alloc(&s->arena, sizeof(an_obj *) * (size_t)nlines, alignof(an_obj *));
    g->u.code.texts = ani_arena_alloc(&s->arena, sizeof(an_obj *) * (size_t)nlines, alignof(an_obj *));
    if (!g->u.code.bars || !g->u.code.texts) return NULL;
    g->u.code.nlines = nlines;
    g->u.code.first_line = first;
    g->u.code.nanchors = nanch;
    g->u.code.anchor_names = ani_arena_alloc(&s->arena, sizeof(char *) * (size_t)(nanch ? nanch : 1), alignof(char *));
    g->u.code.anchor_lines = ani_arena_alloc(&s->arena, sizeof(int) * (size_t)(nanch ? nanch : 1), alignof(int));
    if (!g->u.code.anchor_names || !g->u.code.anchor_lines) return NULL;
    for (int k = 0; k < nanch; k++) {
        g->u.code.anchor_names[k] = anames[k];
        g->u.code.anchor_lines[k] = alines[k];
    }

    // the grid: an advance per column (the mono's '0'), whole-pixel line pitch
    const ani_glyph *zero = ani_font_glyph(f, ani_font_index(f, '0'));
    if (!zero) return NULL;
    const float adv = (float)zero->advance * k;
    const float pitch = roundf((float)(f->ascent - f->descent + f->line_gap) * k);
    const float asc = (float)f->ascent * k, desc = (float)f->descent * k;
    int digits = 1;
    for (int v = first + nlines - 1; v >= 10; v /= 10) digits++;
    const float pad_x = adv, pad_y = roundf(pitch * 0.5f);
    const float gutter = o.line_numbers ? (float)(digits + 2) * adv : 0.0f;
    const float code_x = pad_x + gutter;
    const float base0 = pad_y + roundf(asc);
    g->u.code.base0 = base0;
    g->u.code.pitch = pitch;
    g->u.code.asc = asc;
    g->u.code.desc = desc;
    g->u.code.code_x = code_x;
    g->u.code.adv = adv;

    // widest line, in columns (tabs are 4)
    int cols = 1, c = 0;
    for (const char *p = code;; p++) {
        if (*p == '\n' || !*p) {
            if (c > cols) cols = c;
            c = 0;
            if (!*p) break;
        } else if ((*p & 0xC0) != 0x80) {
            c += *p == '\t' ? 4 : 1;
        }
    }
    const float code_w = (float)cols * adv;
    g->u.code.code_w = code_w;
    const float box_w = code_x + code_w + pad_x, box_h = pad_y * 2.0f + (float)nlines * pitch;

    const an_color ink = pal->ink, paper = s->background;
    if (!o.no_box) {
        an_obj *box = an_rect(s, &(an_rect_opts){
            .parent = g, .z = -2, .pos = { box_w * 0.5f, box_h * 0.5f }, .size = { box_w, box_h },
            .fill = o.box_fill.a > 0.0f ? o.box_fill : paper, .stroke = o.stroke.a > 0.0f ? o.stroke : ink,
            .stroke_width = o.stroke_width > 0.0f ? o.stroke_width : 4.0f });
        if (!box) return NULL;
        ani_obj_part(box, "box", 0);
    }
    const text_style code_style = {
        .fill = o.fill.a > 0.0f ? o.fill : ink, .riso = o.riso == AN_AUTO ? AN_OFF : o.riso,
        .shadow = o.shadow == AN_AUTO ? AN_OFF : o.shadow,
    };
    text_style num_style = code_style;
    num_style.fill = o.number_color.a > 0.0f ? o.number_color : mix(ink, paper, 0.45f);
    const text_params tp = { .font = f, .size = size, .kerning = true, .size_chaos = true, .line_pitch = pitch };
    text_params np = tp;
    np.align = AN_ALIGN_RIGHT;

    char *buf = malloc(strlen(code) + 1);
    if (!buf) return NULL;
    const char *p = code;
    bool ok = true;
    for (int i = 0; ok && i < nlines; i++) {
        const char *e = strchr(p, '\n');
        const size_t len = e ? (size_t)(e - p) : strlen(p);
        memcpy(buf, p, len);
        buf[len] = 0;
        p = e ? e + 1 : p + len;
        const float base = base0 + (float)i * pitch;
        // the bar covers the line box: ascent to descent around the baseline
        const float top = base - asc, bot = base - desc;
        an_obj *bar = an_rect(s, &(an_rect_opts){
            .parent = g, .z = -1, .hidden = true, .pos = { code_x + code_w * 0.5f, (top + bot) * 0.5f },
            .size = { code_w + adv, bot - top }, .fill = o.highlight.a > 0.0f ? o.highlight : pal->c });
        an_obj *num = NULL;
        if (bar && o.line_numbers) {
            char lab[16];
            snprintf(lab, sizeof lab, "%d", first + i);
            num = make_text(s, &(ani_common_opts){ .parent = g, .pos = { pad_x + (float)digits * adv, base } }, lab, &np,
                            &num_style);
        }
        an_obj *txt = bar && (num || !o.line_numbers)
            ? make_text(s, &(ani_common_opts){ .parent = g, .pos = { code_x, base } }, buf, &tp, &code_style)
            : NULL;
        ok = txt != NULL;
        if (bar) ani_obj_part(bar, "bar", i);
        if (num) ani_obj_part(num, "number", i);
        if (txt) ani_obj_part(txt, "line", i);
        if (ok) {
            g->u.code.bars[i] = bar;
            g->u.code.texts[i] = txt;
        }
    }
    free(buf);
    return ok ? g : NULL;
}

an_obj *an_code_line(const an_obj *code, int n) {
    if (!code || code->kind != AN_KIND_CODE) return NULL;
    const int i = n - code->u.code.first_line;
    return i >= 0 && i < code->u.code.nlines ? code->u.code.bars[i] : NULL;
}

an_obj *an_code_text(const an_obj *code, int n) {
    if (!code || code->kind != AN_KIND_CODE) return NULL;
    const int i = n - code->u.code.first_line;
    return i >= 0 && i < code->u.code.nlines ? code->u.code.texts[i] : NULL;
}

// ---- the current-line marker (docs/explainer.md) -------------------------------------------

an_obj *an_code_marker(an_obj *code, const an_code_marker_opts *opts) {
    if (!code || code->kind != AN_KIND_CODE) return NULL;
    if (code->u.code.marker) return code->u.code.marker;
    an_scene *s = code->scene;
    const an_code_marker_opts o = opts ? *opts : (an_code_marker_opts){ 0 };
    const an_palette *pal = s->style.palette;
    const float asc = code->u.code.asc, desc = code->u.code.desc, adv = code->u.code.adv;
    // the group sits at line first_line's baseline; its children are relative to it
    an_obj *m = an_group(s, &(an_group_opts){ .parent = code, .hidden = true, .z = -1,
                                              .pos = { 0, code->u.code.base0 } });
    if (!m) return NULL;
    ani_obj_part(m, "marker", 0);
    const float top = -asc, bot = -desc;
    an_obj *bar = an_rect(s, &(an_rect_opts){
        .parent = m, .pos = { code->u.code.code_x + code->u.code.code_w * 0.5f, (top + bot) * 0.5f },
        .size = { code->u.code.code_w + adv, bot - top }, .fill = o.color.a > 0.0f ? o.color : pal->b });
    // a filled triangle (an_polygon) left of the block, pointing at the line
    const float mid = (top + bot) * 0.5f;
    an_obj *tri = an_polygon(s, &(an_polygon_opts){
        .parent = m, .fill = pal->ink, .points = (an_vec2[]){ { -10, mid }, { -32, mid - 11 }, { -32, mid + 11 } }, .count = 3 });
    if (!bar || !tri) return NULL;
    ani_obj_part(bar, "bar", 0);
    ani_obj_part(tri, "triangle", 0);
    code->u.code.marker = m;
    return m;
}

an_anim an_code_goto(an_obj *code, int line, const an_anim_opts *opts) {
    an_anim a = { .obj = code, .verb = AN_VERB_CODE_GOTO, .swap_i = line };
    if (opts) a.opts = *opts;
    return a;
}

int an_code_current_line(const an_obj *code) {
    return code && code->kind == AN_KIND_CODE ? code->u.code.current : 0;
}

int64_t ani_code_record_goto(an_scene *s, const an_anim *a, int64_t start) {
    an_obj *code = a->obj;
    if (code->kind != AN_KIND_CODE) return start;
    const int line = a->swap_i, i = line - code->u.code.first_line;
    if (i < 0 || i >= code->u.code.nlines) {
        fprintf(stderr, "anown: an_code_goto: line %d isn't in the block\n", line);
        return start;
    }
    an_obj *m = an_code_marker(code, NULL);
    if (!m) return start;
    const ani_value to = { { 0, code->u.code.base0 + (float)i * code->u.code.pitch } };
    int64_t end;
    if (code->u.code.current == 0) { // the first goto: appear at the line, no slide
        ani_record(s, ani_obj_prop(m, AN_PROP_POS), start, 0, AN_EASE_LINEAR, to, false, NULL);
        const int64_t d = ani_ticks(a->opts.duration > 0.0f ? a->opts.duration : 0.25);
        end = ani_record(s, ani_obj_prop(m, AN_PROP_OPACITY), start, d, a->opts.ease, (ani_value){ { 1 } }, false, NULL);
    } else {
        const int64_t d = ani_ticks(a->opts.duration > 0.0f ? a->opts.duration : 0.35);
        an_ease e = a->opts.ease;
        if (e.kind == AN_EASE_KIND_SPRING) e = AN_EASE_IN_OUT; // a spring would overshoot into the next line
        end = ani_record(s, ani_obj_prop(m, AN_PROP_POS), start, d, e, to, false, NULL);
    }
    code->u.code.current = line;
    return end;
}

int an_code_anchor(const an_obj *code, const char *name) {
    if (!code || code->kind != AN_KIND_CODE || !name) return 0;
    for (int k = 0; k < code->u.code.nanchors; k++) {
        if (strcmp(code->u.code.anchor_names[k], name) == 0) return code->u.code.anchor_lines[k];
    }
    fprintf(stderr, "anown: an_code_anchor: no line is marked //@%s\n", name);
    return 0;
}

an_anim an_code_goto_anchor(an_obj *code, const char *name, const an_anim_opts *opts) {
    return an_code_goto(code, an_code_anchor(code, name), opts);
}
