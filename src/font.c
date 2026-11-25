// Fonts: loading (built-in or from a file), metrics, and the glyph cache.
// docs/text.md has the design.
#include "font_internal.h"
#include "scene_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// src/fonts.S
extern const unsigned char ani_font_display_data[], ani_font_display_end[];
extern const unsigned char ani_font_mono_data[], ani_font_mono_end[];

// Vertical stem of 'H' (or 'I', 'l'), in font units: the two leftmost distinct x
// positions of the outline's long vertical line segments. Used for the text size
// factor on chaos. Falls back to 8% of the em.
static int measure_stem(const stbtt_fontinfo *info, int upem) {
    static const int probes[3] = { 'H', 'I', 'l' };
    for (int p = 0; p < 3; p++) {
        const int g = stbtt_FindGlyphIndex(info, probes[p]);
        int x0, y0, x1, y1;
        if (g == 0 || !stbtt_GetGlyphBox(info, g, &x0, &y0, &x1, &y1)) continue;
        stbtt_vertex *v = NULL;
        const int n = stbtt_GetGlyphShape(info, g, &v);
        int xs[64], nx = 0;
        for (int i = 1; i < n && nx < 64; i++) {
            if (v[i].type != STBTT_vline) continue;
            if (v[i].x == v[i - 1].x && abs(v[i].y - v[i - 1].y) * 3 > (y1 - y0)) {
                bool seen = false;
                for (int k = 0; k < nx; k++) seen = seen || xs[k] == v[i].x;
                if (!seen) xs[nx++] = v[i].x;
            }
        }
        stbtt_FreeShape(info, v);
        if (nx < 2) continue;
        for (int a = 1; a < nx; a++) { // insertion sort
            const int t = xs[a];
            int b = a - 1;
            while (b >= 0 && xs[b] > t) { xs[b + 1] = xs[b]; b--; }
            xs[b + 1] = t;
        }
        if (xs[1] > xs[0]) return xs[1] - xs[0];
    }
    return upem * 8 / 100;
}

static an_font *font_make(an_scene *s, const unsigned char *data, const char *name) {
    an_font *f = ANI_NEW(&s->arena, an_font);
    if (!f) return NULL;
    f->scene = s;
    f->data = data;
    f->name = name;
    const int off = stbtt_GetFontOffsetForIndex(data, 0);
    if (off < 0 || !stbtt_InitFont(&f->info, data, off)) {
        fprintf(stderr, "anown: '%s' is not a font stb_truetype can read\n", name);
        return NULL;
    }
    const float one = stbtt_ScaleForMappingEmToPixels(&f->info, 1.0f);
    f->upem = one > 0.0f ? (int)lroundf(1.0f / one) : 1000;
    stbtt_GetFontVMetrics(&f->info, &f->ascent, &f->descent, &f->line_gap);
    f->stem = measure_stem(&f->info, f->upem);
    int x0, y0, x1, y1;
    const int gx = stbtt_FindGlyphIndex(&f->info, 'x');
    f->x_height = (gx && stbtt_GetGlyphBox(&f->info, gx, &x0, &y0, &x1, &y1)) ? y1 : f->upem / 2;
    f->num_glyphs = f->info.numGlyphs;
    f->cache = ani_arena_alloc(&s->arena, sizeof(ani_glyph *) * (size_t)(f->num_glyphs > 0 ? f->num_glyphs : 1),
                               alignof(ani_glyph *));
    return f->cache ? f : NULL;
}

an_font *an_font_display(an_scene *s) {
    if (!s) return NULL;
    if (!s->font_display) s->font_display = font_make(s, ani_font_display_data, "Archivo Black (built in)");
    return s->font_display;
}

an_font *an_font_mono(an_scene *s) {
    if (!s) return NULL;
    if (!s->font_mono) s->font_mono = font_make(s, ani_font_mono_data, "JetBrains Mono (built in)");
    return s->font_mono;
}

an_font *an_font_load(an_scene *s, const char *path) {
    if (!s || !path) return NULL;
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "anown: can't open font '%s'\n", path);
        return NULL;
    }
    unsigned char *data = NULL;
    long size = -1;
    if (fseek(fp, 0, SEEK_END) == 0) size = ftell(fp);
    if (size > 0 && fseek(fp, 0, SEEK_SET) == 0) {
        data = ani_arena_alloc(&s->arena, (size_t)size, 16);
        if (data && fread(data, 1, (size_t)size, fp) != (size_t)size) data = NULL;
    }
    fclose(fp);
    if (!data) {
        fprintf(stderr, "anown: can't read font '%s'\n", path);
        return NULL;
    }
    return font_make(s, data, ani_arena_strdup(&s->arena, path));
}

int ani_font_index(const an_font *f, uint32_t cp) {
    return stbtt_FindGlyphIndex(&f->info, (int)cp);
}

int ani_font_kern(const an_font *f, int g1, int g2) {
    return stbtt_GetGlyphKernAdvance(&f->info, g1, g2);
}

const ani_glyph *ani_font_glyph(an_font *f, int gi) {
    if (gi < 0 || gi >= f->num_glyphs) gi = 0;
    if (f->cache[gi]) return f->cache[gi];
    ani_glyph *g = ANI_NEW(&f->scene->arena, ani_glyph);
    if (!g) return NULL;
    g->index = gi;
    stbtt_GetGlyphHMetrics(&f->info, gi, &g->advance, &g->lsb);
    if (!stbtt_GetGlyphBox(&f->info, gi, &g->x0, &g->y0, &g->x1, &g->y1)) g->x0 = g->y0 = g->x1 = g->y1 = 0;
    stbtt_vertex *v = NULL;
    const int n = stbtt_GetGlyphShape(&f->info, gi, &v);
    if (n > 0) {
        // 1 / (1000 x max_zoom) em: 1/4000 at the default max_zoom (docs/camera.md)
        const float tol = (float)f->upem / (1000.0f * f->scene->max_zoom);
        ani_path_builder b;
        ani_pb_init(&b);
        bool ok = true;
        for (int i = 0; ok && i < n; i++) {
            const an_vec2 p = { (float)v[i].x, -(float)v[i].y };
            switch (v[i].type) {
            case STBTT_vmove:
                ani_pb_close(&b); // the previous contour (no-op for the first)
                ok = ani_pb_move(&b, p);
                break;
            case STBTT_vline: ok = ani_pb_line(&b, p); break;
            case STBTT_vcurve: ok = ani_pb_quad(&b, (an_vec2){ (float)v[i].cx, -(float)v[i].cy }, p, tol); break;
            case STBTT_vcubic:
                ok = ani_pb_cubic(&b, (an_vec2){ (float)v[i].cx, -(float)v[i].cy },
                                  (an_vec2){ (float)v[i].cx1, -(float)v[i].cy1 }, p, tol);
                break;
            default: break;
            }
        }
        ani_pb_close(&b);
        g->path = ok ? ani_pb_finish(&b, &f->scene->arena) : NULL;
        ani_pb_free(&b);
        stbtt_FreeShape(&f->info, v);
        if (!ok || !g->path) return NULL;
    }
    f->cache[gi] = g;
    return g;
}
