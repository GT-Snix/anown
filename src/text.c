// Text and code blocks: layout once at creation, one object per letter.
// docs/text.md is the spec.
#include "text_internal.h"
#include "font_internal.h"
#include "scene_internal.h"

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

static an_obj *make_text(an_scene *s, const ani_common_opts *node, const char *str, const text_params *tp,
                         const text_style *ts) {
    an_font *f = tp->font;
    const char *copy = ani_arena_strdup(&s->arena, str);
    if (!f || !copy) return NULL;
    const float k = tp->size / (float)f->upem;
    layout L = { 0 };
    if (!layout_run(&L, f, copy, k, tp->kerning)) {
        free(L.g);
        free(L.line_w);
        return NULL;
    }
    // the node carries the letters' paint too: letters follow its fill, stroke
    // and stroke width until overridden (docs/text.md)
    ani_common_opts nopts = *node;
    nopts.fill = ts->fill;
    nopts.stroke = ts->stroke;
    nopts.stroke_width = ts->stroke_width;
    an_obj *t = ani_obj_make(s, AN_KIND_TEXT, &nopts);
    an_obj **glyphs = NULL;
    int ng = 0;
    for (int i = 0; i < L.n; i++) ng += ani_font_glyph(f, L.g[i].gi)->path != NULL;
    if (t) glyphs = ani_arena_alloc(&s->arena, sizeof(an_obj *) * (size_t)(ng ? ng : 1), alignof(an_obj *));
    if (!t || !glyphs) {
        free(L.g);
        free(L.line_w);
        return NULL;
    }
    t->u.text.str = copy;
    t->u.text.size = tp->size;
    t->u.text.font = f;
    t->u.text.size_chaos = tp->size_chaos;
    t->u.text.inked = ts->outline;

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
    int gi_out = 0;
    for (int i = 0; i < L.n; i++) {
        const ani_glyph *g = ani_font_glyph(f, L.g[i].gi);
        if (!g->path) continue; // a space: no letter object
        const float lw = L.line_w[L.g[i].line];
        const float ax = tp->align == AN_ALIGN_CENTER ? -lw * 0.5f : tp->align == AN_ALIGN_RIGHT ? -lw : 0.0f;
        const an_vec2 pen = { L.g[i].x + ax, y0 + (float)L.g[i].line * pitch };
        const an_vec2 pivot = { (float)g->advance * k * 0.5f, -xh * 0.5f };
        ani_path *path = glyph_path(&s->arena, g->path, k, pivot);
        an_obj *o = path ? ani_obj_make(s, AN_KIND_GLYPH, &(ani_common_opts){
                               .parent = t, .pos = { pen.x + pivot.x, pen.y + pivot.y }, .hidden = ts->hidden,
                               .undrawn = ts->undrawn, .fill = ts->fill, .stroke = ts->stroke,
                               .stroke_width = ts->stroke_width, .no_fill = ts->no_fill, .brush = ts->brush,
                               .riso = ts->riso, .shadow = ts->shadow, .pattern = ts->pattern,
                               .paper_under = ts->paper_under })
                         : NULL;
        if (!o) {
            free(L.g);
            free(L.line_w);
            return NULL;
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
    // the verb pivot: the center of the block (advance widths x ascent to descent)
    float bx0 = 0.0f, bx1 = 0.0f;
    for (int l = 0; l < L.nlines; l++) {
        const float lw = L.line_w[l];
        const float ax = tp->align == AN_ALIGN_CENTER ? -lw * 0.5f : tp->align == AN_ALIGN_RIGHT ? -lw : 0.0f;
        if (l == 0 || ax < bx0) bx0 = ax;
        if (l == 0 || ax + lw > bx1) bx1 = ax + lw;
    }
    t->vpivot = (an_vec2){ (bx0 + bx1) * 0.5f, ((y0 - asc) + (y0 + last - desc)) * 0.5f };
    t->u.text.glyphs = glyphs;
    t->u.text.nglyphs = gi_out;
    t->u.text.total_arc = arc;
    free(L.g);
    free(L.line_w);
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

int an_text_length(const an_obj *t) {
    return t && t->kind == AN_KIND_TEXT ? t->u.text.nglyphs : 0;
}

an_obj *an_text_glyph(const an_obj *t, int i) {
    if (!t || t->kind != AN_KIND_TEXT || i < 0 || i >= t->u.text.nglyphs) return NULL;
    return t->u.text.glyphs[i];
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
    const char *code = o.code ? o.code : "";

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
    const float box_w = code_x + code_w + pad_x, box_h = pad_y * 2.0f + (float)nlines * pitch;

    const an_color ink = pal->ink, paper = s->background;
    if (!o.no_box) {
        an_obj *box = an_rect(s, &(an_rect_opts){
            .parent = g, .z = -2, .pos = { box_w * 0.5f, box_h * 0.5f }, .size = { box_w, box_h },
            .fill = o.box_fill.a > 0.0f ? o.box_fill : paper, .stroke = o.stroke.a > 0.0f ? o.stroke : ink,
            .stroke_width = o.stroke_width > 0.0f ? o.stroke_width : 4.0f });
        if (!box) return NULL;
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
