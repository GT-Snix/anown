// Rendering: direct paints, isolated layers with stencil coverage, and
// premultiplied compositing. docs/layers.md is the spec.
#include "render_internal.h"

#include "composite.glsl.h"
#include "paper.glsl.h"
#include "shape.glsl.h"
#include "timeline_internal.h"

#include <string.h>
#include <time.h>

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#define LAYER_COLOR_FMT SG_PIXELFORMAT_RGBA8
#define LAYER_DS_FMT SG_PIXELFORMAT_DEPTH_STENCIL
#define LAYER_MARGIN 2

static bool grow(void **p, int *cap, int need, size_t elem) {
    if (need <= *cap) return true;
    int n = *cap ? *cap : 16;
    while (n < need) n *= 2;
    void *q = realloc(*p, (size_t)n * elem);
    if (!q) return false;
    *p = q;
    *cap = n;
    return true;
}

// ---- pipelines -----------------------------------------------------------------------

// Paint blend: straight-alpha source over the target. Over a transparent layer
// this stores premultiplied color; over premultiplied content it's exactly
// premultiplied "over". Destination alpha accumulates as a + d(1 - a).
static const sg_blend_state PAINT_BLEND = {
    .enabled = true,
    .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
    .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
    .src_factor_alpha = SG_BLENDFACTOR_ONE,
    .dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
};

// Composite blend: the layer is premultiplied.
static const sg_blend_state COMP_BLEND = {
    .enabled = true,
    .src_factor_rgb = SG_BLENDFACTOR_ONE,
    .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
    .src_factor_alpha = SG_BLENDFACTOR_ONE,
    .dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
};

static sg_pipeline make_shape_pip(sg_shader shd, sg_pixel_format cf, sg_pixel_format df, int samples,
                                  const sg_stencil_state *stencil, bool color_off, const char *label) {
    return sg_make_pipeline(&(sg_pipeline_desc){
        .shader = shd,
        .layout = {
            .buffers[0].stride = sizeof(ani_vertex),
            .attrs = {
                [ATTR_shape_pos] = { .format = SG_VERTEXFORMAT_FLOAT2, .offset = offsetof(ani_vertex, x) },
                [ATTR_shape_color0] = { .format = SG_VERTEXFORMAT_FLOAT4, .offset = offsetof(ani_vertex, r) },
                [ATTR_shape_pattern0] = { .format = SG_VERTEXFORMAT_FLOAT3, .offset = offsetof(ani_vertex, pu) },
            },
        },
        .colors[0] = {
            .pixel_format = cf,
            .write_mask = color_off ? SG_COLORMASK_NONE : SG_COLORMASK_RGBA,
            .blend = color_off ? (sg_blend_state){ 0 } : PAINT_BLEND,
        },
        .depth.pixel_format = df, // compare defaults to ALWAYS, no writes
        .stencil = stencil ? *stencil : (sg_stencil_state){ 0 },
        .sample_count = samples,
        .label = label,
    });
}

// A stencil state: compare (stencil & read) with (ref & read); on pass, apply `pass`
// to the bits in `write`. Layout: 0x80 = silhouette bit, 0x7F = write-once marks.
static sg_stencil_state stencil(sg_compare_func cmp, sg_stencil_op pass, uint8_t ref, uint8_t read, uint8_t write) {
    const sg_stencil_face_state f = { .compare = cmp, .fail_op = SG_STENCILOP_KEEP,
                                      .depth_fail_op = SG_STENCILOP_KEEP, .pass_op = pass };
    return (sg_stencil_state){ .enabled = true, .front = f, .back = f, .read_mask = read, .write_mask = write, .ref = ref };
}

// The fixed stencil states (docs/layers.md):
//   silhouette set/clear: always; write the 0x80 bit to 1 / 0 (color off)
//   shadow cut:    pass where the silhouette bit is 0; write nothing (opaque
//                  shadows, main or layer)
//   layer shadow:  pass where 0x7F > stencil, i.e. outside the silhouette (bit
//                  0x80 clear) and not yet shadowed (low bits != 0x7F); then
//                  write 0x7F to the low bits: write-once, as before
//   layer fill/ink: write-once on the low bits only (refs 2 and 3)
//   stencil clear: always; all bits to 0 (color off)
#define ST_SIL      stencil(SG_COMPAREFUNC_ALWAYS, SG_STENCILOP_REPLACE, 0x80, 0xFF, 0x80)
#define ST_SILCLR   stencil(SG_COMPAREFUNC_ALWAYS, SG_STENCILOP_REPLACE, 0x00, 0xFF, 0x80)
#define ST_SHADOW_M stencil(SG_COMPAREFUNC_EQUAL, SG_STENCILOP_KEEP, 0x00, 0x80, 0x00)
#define ST_SHADOW_L stencil(SG_COMPAREFUNC_GREATER, SG_STENCILOP_REPLACE, 0x7F, 0xFF, 0x7F)
#define ST_FILL_L   stencil(SG_COMPAREFUNC_NOT_EQUAL, SG_STENCILOP_REPLACE, 0x02, 0x7F, 0x7F)
#define ST_INK_L    stencil(SG_COMPAREFUNC_NOT_EQUAL, SG_STENCILOP_REPLACE, 0x03, 0x7F, 0x7F)
#define ST_CLEAR_L  stencil(SG_COMPAREFUNC_ALWAYS, SG_STENCILOP_REPLACE, 0x00, 0xFF, 0xFF)

static sg_pipeline make_comp_pip(sg_shader shd, sg_pixel_format cf, sg_pixel_format df, int samples, const char *label) {
    return sg_make_pipeline(&(sg_pipeline_desc){
        .shader = shd,
        .colors[0] = { .pixel_format = cf, .blend = COMP_BLEND },
        .depth.pixel_format = df,
        .sample_count = samples,
        .label = label,
    });
}

bool ani_renderer_init(ani_renderer *r, sg_pixel_format color_format,
                       sg_pixel_format depth_format, int sample_count) {
    *r = (ani_renderer){ 0 };
    r->budget = ANI_LAYER_BUDGET;
    ani_mesh_init(&r->mesh);
    r->shape_shd = sg_make_shader(shape_shader_desc(sg_query_backend()));
    r->comp_shd = sg_make_shader(composite_shader_desc(sg_query_backend()));
    r->smp = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_NEAREST, .mag_filter = SG_FILTER_NEAREST,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE, .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
    });

    r->pip_main = make_shape_pip(r->shape_shd, color_format, depth_format, sample_count, NULL, false, "anown-main");
    r->pip_comp_main = make_comp_pip(r->comp_shd, color_format, depth_format, sample_count, "anown-comp-main");
    r->paper_shd = sg_make_shader(paper_shader_desc(sg_query_backend()));
    r->pip_paper = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = r->paper_shd,
        .layout = {
            .buffers[0].stride = sizeof(ani_vertex), // the shared vertex buffer
            .attrs = {
                [ATTR_paper_pos] = { .format = SG_VERTEXFORMAT_FLOAT2, .offset = offsetof(ani_vertex, x) },
                [ATTR_paper_color0] = { .format = SG_VERTEXFORMAT_FLOAT4, .offset = offsetof(ani_vertex, r) },
            },
        },
        .colors[0].pixel_format = color_format, // opaque: no blending
        .depth.pixel_format = depth_format,
        .sample_count = sample_count,
        .label = "anown-paper",
    });

    // main target: shadow masking needs its depth-stencil buffer
    r->main_stencil = depth_format == SG_PIXELFORMAT_DEPTH_STENCIL;
    if (r->main_stencil) {
        const sg_stencil_state sm = ST_SHADOW_M, ss = ST_SIL, sc = ST_SILCLR;
        r->pip_main_shadow = make_shape_pip(r->shape_shd, color_format, depth_format, sample_count, &sm, false, "anown-main-shadow");
        r->pip_main_sil = make_shape_pip(r->shape_shd, color_format, depth_format, sample_count, &ss, true, "anown-main-silhouette");
        r->pip_main_silclr = make_shape_pip(r->shape_shd, color_format, depth_format, sample_count, &sc, true, "anown-main-silhouette-clear");
    }
    const sg_stencil_state s1 = ST_SHADOW_L, s2 = ST_FILL_L, s3 = ST_INK_L, s0 = ST_CLEAR_L, sl = ST_SIL, slc = ST_SILCLR;
    r->pip_layer[ANI_PAINT_DIRECT] = make_shape_pip(r->shape_shd, LAYER_COLOR_FMT, LAYER_DS_FMT, ANI_LAYER_SAMPLES, NULL, false, "anown-layer-direct");
    r->pip_layer[ANI_PAINT_SHADOW] = make_shape_pip(r->shape_shd, LAYER_COLOR_FMT, LAYER_DS_FMT, ANI_LAYER_SAMPLES, &s1, false, "anown-layer-shadow");
    r->pip_layer[ANI_PAINT_FILL] = make_shape_pip(r->shape_shd, LAYER_COLOR_FMT, LAYER_DS_FMT, ANI_LAYER_SAMPLES, &s2, false, "anown-layer-fill");
    r->pip_layer[ANI_PAINT_INK] = make_shape_pip(r->shape_shd, LAYER_COLOR_FMT, LAYER_DS_FMT, ANI_LAYER_SAMPLES, &s3, false, "anown-layer-ink");
    r->pip_layer[ANI_PAINT_STENCIL_CLEAR] = make_shape_pip(r->shape_shd, LAYER_COLOR_FMT, LAYER_DS_FMT, ANI_LAYER_SAMPLES, &s0, true, "anown-layer-stencil-clear");
    r->pip_layer[ANI_PAINT_SILHOUETTE] = make_shape_pip(r->shape_shd, LAYER_COLOR_FMT, LAYER_DS_FMT, ANI_LAYER_SAMPLES, &sl, true, "anown-layer-silhouette");
    r->pip_layer[ANI_PAINT_SILHOUETTE_CLEAR] = make_shape_pip(r->shape_shd, LAYER_COLOR_FMT, LAYER_DS_FMT, ANI_LAYER_SAMPLES, &slc, true, "anown-layer-silhouette-clear");
    const sg_stencil_state sh_cut = ST_SHADOW_M;
    r->pip_layer[ANI_PAINT_SHADOW_CUT] = make_shape_pip(r->shape_shd, LAYER_COLOR_FMT, LAYER_DS_FMT, ANI_LAYER_SAMPLES, &sh_cut, false, "anown-layer-shadow-cut");
    r->pip_comp_layer = make_comp_pip(r->comp_shd, LAYER_COLOR_FMT, LAYER_DS_FMT, ANI_LAYER_SAMPLES, "anown-comp-layer");

    bool ok = sg_query_pipeline_state(r->pip_main) == SG_RESOURCESTATE_VALID &&
              sg_query_pipeline_state(r->pip_comp_main) == SG_RESOURCESTATE_VALID &&
              sg_query_pipeline_state(r->pip_paper) == SG_RESOURCESTATE_VALID &&
              sg_query_pipeline_state(r->pip_comp_layer) == SG_RESOURCESTATE_VALID;
    for (int i = 0; i < ANI_PAINT_COUNT; i++) {
        if (i == ANI_PAINT_BACKGROUND) continue; // main only
        ok = ok && sg_query_pipeline_state(r->pip_layer[i]) == SG_RESOURCESTATE_VALID;
    }
    if (r->main_stencil) {
        ok = ok && sg_query_pipeline_state(r->pip_main_shadow) == SG_RESOURCESTATE_VALID &&
             sg_query_pipeline_state(r->pip_main_sil) == SG_RESOURCESTATE_VALID &&
             sg_query_pipeline_state(r->pip_main_silclr) == SG_RESOURCESTATE_VALID;
    }
    return ok;
}

void ani_renderer_shutdown(ani_renderer *r) {
    for (int i = 0; i < r->nscratch; i++) {
        sg_destroy_view(r->scratches[i].color_view);
        sg_destroy_view(r->scratches[i].ds_view);
        sg_destroy_image(r->scratches[i].color);
        sg_destroy_image(r->scratches[i].ds);
    }
    for (int i = 0; i < r->nresolve; i++) {
        sg_destroy_view(r->resolves[i].resolve_view);
        sg_destroy_view(r->resolves[i].tex_view);
        sg_destroy_image(r->resolves[i].img);
    }
    free(r->resolves);
    free(r->rects);
    sg_destroy_buffer(r->vbuf);
    sg_destroy_pipeline(r->pip_main);
    sg_destroy_pipeline(r->pip_comp_main);
    sg_destroy_pipeline(r->pip_paper);
    sg_destroy_shader(r->paper_shd);
    for (int i = 0; i < ANI_PAINT_COUNT; i++) sg_destroy_pipeline(r->pip_layer[i]);
    sg_destroy_pipeline(r->pip_main_shadow);
    sg_destroy_pipeline(r->pip_main_sil);
    sg_destroy_pipeline(r->pip_main_silclr);
    sg_destroy_pipeline(r->pip_comp_layer);
    sg_destroy_shader(r->shape_shd);
    sg_destroy_shader(r->comp_shd);
    sg_destroy_sampler(r->smp);
    ani_mesh_free(&r->mesh);
    *r = (ani_renderer){ 0 };
}

// ---- pool ----------------------------------------------------------------------------

static int class_of(int v, int cap) {
    int c = (v + ANI_LAYER_CLASS - 1) / ANI_LAYER_CLASS * ANI_LAYER_CLASS;
    const int capc = (cap + ANI_LAYER_CLASS - 1) / ANI_LAYER_CLASS * ANI_LAYER_CLASS;
    return c > capc ? capc : c;
}

// 4 samples x (4 B color + 8 B Depth32Float_Stencil8)
static size_t scratch_bytes(int w, int h) { return (size_t)w * (size_t)h * 48; }
static size_t resolve_bytes(int w, int h) { return (size_t)w * (size_t)h * 4; }

static int get_scratch(ani_renderer *r, int w, int h) {
    for (int i = 0; i < r->nscratch; i++) {
        if (r->scratches[i].w == w && r->scratches[i].h == h) return i;
    }
    if (r->nscratch == (int)(sizeof r->scratches / sizeof r->scratches[0])) return -1;
    if (r->pool_bytes + scratch_bytes(w, h) > r->budget) return -1;
    ani_rt_scratch *sc = &r->scratches[r->nscratch];
    sc->w = w;
    sc->h = h;
    sc->color = sg_make_image(&(sg_image_desc){ .usage.color_attachment = true, .width = w, .height = h,
                                                .pixel_format = LAYER_COLOR_FMT, .sample_count = ANI_LAYER_SAMPLES,
                                                .label = "anown-layer-msaa" });
    sc->ds = sg_make_image(&(sg_image_desc){ .usage.depth_stencil_attachment = true, .width = w, .height = h,
                                             .pixel_format = LAYER_DS_FMT, .sample_count = ANI_LAYER_SAMPLES,
                                             .label = "anown-layer-ds" });
    sc->color_view = sg_make_view(&(sg_view_desc){ .color_attachment.image = sc->color });
    sc->ds_view = sg_make_view(&(sg_view_desc){ .depth_stencil_attachment.image = sc->ds });
    if (sg_query_view_state(sc->color_view) != SG_RESOURCESTATE_VALID ||
        sg_query_view_state(sc->ds_view) != SG_RESOURCESTATE_VALID)
        return -1;
    r->pool_bytes += scratch_bytes(w, h);
    return r->nscratch++;
}

static int get_resolve(ani_renderer *r, int w, int h) {
    for (int i = 0; i < r->nresolve; i++) {
        if (!r->resolves[i].in_use && r->resolves[i].w == w && r->resolves[i].h == h) {
            r->resolves[i].in_use = true;
            return i;
        }
    }
    if (r->pool_bytes + resolve_bytes(w, h) > r->budget) return -1;
    if (!grow((void **)&r->resolves, &r->resolve_cap, r->nresolve + 1, sizeof(ani_rt_resolve))) return -1;
    ani_rt_resolve *rs = &r->resolves[r->nresolve];
    *rs = (ani_rt_resolve){ .w = w, .h = h, .in_use = true };
    rs->img = sg_make_image(&(sg_image_desc){ .usage.resolve_attachment = true, .width = w, .height = h,
                                              .pixel_format = LAYER_COLOR_FMT, .sample_count = 1,
                                              .label = "anown-layer-resolve" });
    rs->resolve_view = sg_make_view(&(sg_view_desc){ .resolve_attachment.image = rs->img });
    rs->tex_view = sg_make_view(&(sg_view_desc){ .texture.image = rs->img });
    if (sg_query_view_state(rs->resolve_view) != SG_RESOURCESTATE_VALID) return -1;
    r->pool_bytes += resolve_bytes(w, h);
    return r->nresolve++;
}

// ---- drawing a target's command list ----------------------------------------------------

// The current target: main, or a layer. Pixel origin in global target pixels.
typedef struct target_ctx {
    int index;   // ANI_TARGET_MAIN or layer index
    int ox, oy;  // its rectangle origin in target pixels (0, 0 for main)
    int w, h;    // its size in pixels (main: full target)
} target_ctx;

static void set_shape_view(ani_renderer *r, const target_ctx *t, float alpha_mul) {
    shape_params_t p = { .alpha_mul = alpha_mul };
    if (t->index == ANI_TARGET_MAIN) {
        sg_apply_viewportf(r->vx, r->vy, r->vw, r->vh, true);
        p.view[0] = 0;
        p.view[1] = 0;
        p.view[2] = r->sw;
        p.view[3] = r->sh;
    } else {
        sg_apply_viewport(0, 0, t->w, t->h, true);
        p.view[0] = ((float)t->ox - r->vx) / r->k;
        p.view[1] = ((float)t->oy - r->vy) / r->k;
        p.view[2] = (float)t->w / r->k;
        p.view[3] = (float)t->h / r->k;
    }
    sg_apply_uniforms(UB_shape_params, &SG_RANGE(p));
}

static void draw_target(ani_renderer *r, const target_ctx *t, int list, float alpha_mul, bool replay);

// The background run, drawn with the paper shader (grain, vignette).
static void draw_paper(ani_renderer *r, const ani_cmd *c) {
    sg_apply_pipeline(r->pip_paper);
    sg_apply_bindings(&(sg_bindings){ .vertex_buffers[0] = r->vbuf });
    sg_apply_viewportf(r->vx, r->vy, r->vw, r->vh, true);
    const paper_vs_params_t vs = { .view = { 0, 0, r->sw, r->sh } };
    paper_fs_params_t fs = {
        .paper = { r->paper_color[0], r->paper_color[1], r->paper_color[2], 1 },
        .amounts = { r->grain, r->vignette, r->sw, r->sh },
    };
    // uint bit patterns through int uniforms
    memcpy(&fs.ids[0], &r->seed_lo, 4);
    memcpy(&fs.ids[1], &r->seed_hi, 4);
    memcpy(&fs.ids[2], &r->grain_step, 4);
    sg_apply_uniforms(UB_paper_vs_params, &SG_RANGE(vs));
    sg_apply_uniforms(UB_paper_fs_params, &SG_RANGE(fs));
    sg_draw((uint32_t)c->first, (uint32_t)c->count, 1);
}

// Composite layer `li` into target t (or replay it there if it has no texture).
static void composite(ani_renderer *r, const target_ctx *t, int li, float alpha_mul) {
    const ani_layer_rect *lr = &r->rects[li];
    if (lr->empty) return;
    const float opacity = r->mesh.layers[li].opacity * alpha_mul;
    if (lr->tex < 0) { // over budget: draw its contents straight into this target
        draw_target(r, t, li, opacity, true);
        return;
    }
    const bool main = t->index == ANI_TARGET_MAIN;
    sg_apply_pipeline(main ? r->pip_comp_main : r->pip_comp_layer);
    sg_apply_viewport(0, 0, t->w, t->h, true);
    const float rx = (float)(lr->x - t->ox), ry = (float)(lr->y - t->oy);
    const comp_vs_params_t vs = { .rect = { rx, ry, (float)lr->w, (float)lr->h }, .target = { (float)t->w, (float)t->h } };
    const comp_fs_params_t fs = { .src_origin = { rx, ry }, .opacity = opacity };
    sg_apply_bindings(&(sg_bindings){ .views[VIEW_layer_tex] = r->resolves[lr->tex].tex_view,
                                      .samplers[SMP_layer_smp] = r->smp });
    sg_apply_uniforms(UB_comp_vs_params, &SG_RANGE(vs));
    sg_apply_uniforms(UB_comp_fs_params, &SG_RANGE(fs));
    sg_draw(0, 6, 1);
}

// Draw the commands whose target is `list` into the current pass (target t).
// replay: a fallback layer drawn directly into its parent (no stencil, alpha_mul
// carries its opacity).
static void draw_target(ani_renderer *r, const target_ctx *t, int list, float alpha_mul, bool replay) {
    const bool main = t->index == ANI_TARGET_MAIN;
    for (int i = 0; i < r->mesh.ncmds; i++) {
        const ani_cmd *c = &r->mesh.cmds[i];
        if (c->target != list) continue;
        if (c->kind == ANI_CMD_COMPOSITE) {
            composite(r, t, c->layer, alpha_mul);
            continue;
        }
        if (c->count == 0) continue;
        sg_pipeline pip;
        if (main && c->paint == ANI_PAINT_BACKGROUND && r->paper) {
            draw_paper(r, c);
            continue;
        }
        const bool sil = c->paint == ANI_PAINT_SILHOUETTE || c->paint == ANI_PAINT_SILHOUETTE_CLEAR;
        if (main) {
            if (c->paint == ANI_PAINT_STENCIL_CLEAR) continue; // only in replays
            if (sil && !r->main_stencil) continue;   // no stencil buffer: shadows unmasked
            if (c->paint == ANI_PAINT_SILHOUETTE) pip = r->pip_main_sil;
            else if (c->paint == ANI_PAINT_SILHOUETTE_CLEAR) pip = r->pip_main_silclr;
            else if ((c->paint == ANI_PAINT_SHADOW_CUT || c->paint == ANI_PAINT_SHADOW) && r->main_stencil) pip = r->pip_main_shadow;
            else pip = r->pip_main;
        } else if (replay) {
            // over-budget fallback: drawn straight into the parent, no stencil at all
            if (c->paint == ANI_PAINT_STENCIL_CLEAR || sil) continue;
            pip = r->pip_layer[ANI_PAINT_DIRECT];
        } else {
            pip = r->pip_layer[c->paint];
        }
        sg_apply_pipeline(pip);
        sg_apply_bindings(&(sg_bindings){ .vertex_buffers[0] = r->vbuf });
        set_shape_view(r, t, alpha_mul);
        sg_draw((uint32_t)c->first, (uint32_t)c->count, 1);
    }
}

// ---- frame -------------------------------------------------------------------------------

static void layer_bounds(ani_renderer *r) {
    const ani_mesh *m = &r->mesh;
    // children have higher indices than their parents: go backwards so every
    // child's rectangle is known before its parent's
    for (int li = m->nlayers - 1; li >= 0; li--) {
        float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
        for (int i = 0; i < m->ncmds; i++) {
            const ani_cmd *c = &m->cmds[i];
            if (c->target != li) continue;
            if (c->kind == ANI_CMD_COMPOSITE) {
                const ani_layer_rect *ch = &r->rects[c->layer];
                if (ch->empty) continue;
                x0 = fminf(x0, (float)ch->x);
                y0 = fminf(y0, (float)ch->y);
                x1 = fmaxf(x1, (float)(ch->x + ch->w));
                y1 = fmaxf(y1, (float)(ch->y + ch->h));
            } else if (c->paint != ANI_PAINT_STENCIL_CLEAR) {
                for (int v = c->first; v < c->first + c->count; v++) {
                    const float px = r->vx + m->v[v].x * r->k, py = r->vy + m->v[v].y * r->k;
                    x0 = fminf(x0, px);
                    y0 = fminf(y0, py);
                    x1 = fmaxf(x1, px);
                    y1 = fmaxf(y1, py);
                }
            }
        }
        ani_layer_rect *lr = &r->rects[li];
        *lr = (ani_layer_rect){ .empty = true, .tex = -1, .scratch = -1 };
        if (!(x1 > x0) || !(y1 > y0)) continue;
        // snap outward, add the margin, clip to the letterboxed scene area
        int ix0 = (int)floorf(x0) - LAYER_MARGIN, iy0 = (int)floorf(y0) - LAYER_MARGIN;
        int ix1 = (int)ceilf(x1) + LAYER_MARGIN, iy1 = (int)ceilf(y1) + LAYER_MARGIN;
        const int cx0 = (int)r->vx, cy0 = (int)r->vy;
        const int cx1 = (int)ceilf(r->vx + r->vw), cy1 = (int)ceilf(r->vy + r->vh);
        if (ix0 < cx0) ix0 = cx0;
        if (iy0 < cy0) iy0 = cy0;
        if (ix1 > cx1) ix1 = cx1;
        if (iy1 > cy1) iy1 = cy1;
        if (ix1 <= ix0 || iy1 <= iy0) continue;
        *lr = (ani_layer_rect){ .x = ix0, .y = iy0, .w = ix1 - ix0, .h = iy1 - iy0, .empty = false, .tex = -1, .scratch = -1 };
    }
}

static void ensure_vbuf(ani_renderer *r, size_t bytes) {
    if (bytes <= r->vbuf_bytes) return;
    size_t cap = r->vbuf_bytes ? r->vbuf_bytes : 64 * 1024;
    while (cap < bytes) cap *= 2;
    sg_destroy_buffer(r->vbuf);
    r->vbuf = sg_make_buffer(&(sg_buffer_desc){
        .usage = { .vertex_buffer = true, .write_transient = true },
        .size = cap,
        .label = "anown-shape-vertices",
    });
    r->vbuf_bytes = cap;
}

static double uptime_s(void) { return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) * 1e-9; }

bool ani_renderer_prepare(ani_renderer *r, const an_scene *s, int64_t frame, int tw, int th) {
    r->prof_start = uptime_s();
    // reserve the scene's estimated peak once, so arrays and the GPU buffer don't
    // double partway through a sequence
    if (r->reserved_for != s || r->reserved_count != s->count) {
        size_t cmds;
        size_t est = ani_estimate_vertices(s, &cmds);
        if (est > (size_t)1 << 27) est = (size_t)1 << 27;
        ani_mesh_reserve(&r->mesh, (int)est, (int)cmds);
        ensure_vbuf(r, est * sizeof(ani_vertex));
        r->reserved_for = s;
        r->reserved_count = s->count;
    }
    if (!ani_tess_scene(&r->mesh, s, frame)) return false;
    r->prof_tess = uptime_s();
    const ani_mesh *m = &r->mesh;

    // letterbox: fit the scene inside the target, centered, origin on whole pixels
    r->tw = tw;
    r->th = th;
    r->sw = (float)s->width;
    r->sh = (float)s->height;
    r->k = (float)tw / r->sw < (float)th / r->sh ? (float)tw / r->sw : (float)th / r->sh;
    r->vw = r->sw * r->k;
    r->vh = r->sh * r->k;
    r->vx = floorf(((float)tw - r->vw) * 0.5f);
    r->vy = floorf(((float)th - r->vh) * 0.5f);

    // vertices: one upload for the whole frame, before anything binds the buffer
    const size_t bytes = (size_t)m->count * sizeof(ani_vertex);
    ensure_vbuf(r, bytes);
    sg_write_buffer_transient(&(sg_write_buffer_desc){
        .src.data = { .ptr = m->v, .size = bytes ? bytes : sizeof(ani_vertex) },
        .dst.buffer = r->vbuf,
    });
    r->prof_upload = uptime_s();

    // paper background: grain and vignette scale with the scene's chaos at this frame;
    // grain steps at 12 Hz unless asked for every frame ((N * 12) / fps, integer)
    {
        const an_style *st = &s->style;
        const float chaos = (float)ani_scene_chaos_at((an_scene *)s, ani_frame_time(s, frame)); // reads only
        r->grain = st->grain_off ? 0.0f : st->grain * chaos;
        r->vignette = st->vignette_off ? 0.0f : st->vignette * chaos;
        r->paper = st->background == AN_BG_PAPER && (r->grain > 0.0f || r->vignette > 0.0f);
        r->paper_color[0] = s->background.r;
        r->paper_color[1] = s->background.g;
        r->paper_color[2] = s->background.b;
        r->paper_color[3] = 1.0f;
        r->seed_lo = (uint32_t)s->seed;
        r->seed_hi = (uint32_t)(s->seed >> 32);
        const int64_t f = frame < 0 ? 0 : frame;
        r->grain_step = (uint32_t)(st->grain_every_frame ? f : (f * 12) / s->fps);
    }

    // layers: bounds, memory, then render deepest first
    r->layers_rendered = r->layers_fallback = 0;
    if (!grow((void **)&r->rects, &r->rect_cap, m->nlayers ? m->nlayers : 1, sizeof(ani_layer_rect))) return false;
    for (int i = 0; i < r->nresolve; i++) r->resolves[i].in_use = false;
    layer_bounds(r);
    for (int li = 0; li < m->nlayers; li++) {
        ani_layer_rect *lr = &r->rects[li];
        if (lr->empty) continue;
        const int wc = class_of(lr->w, tw), hc = class_of(lr->h, th);
        lr->scratch = get_scratch(r, wc, hc);
        lr->tex = lr->scratch >= 0 ? get_resolve(r, wc, hc) : -1;
        if (lr->tex < 0) {
            r->layers_fallback++;
            if (!r->warned_budget) {
                fprintf(stderr, "anown: layer memory budget (%zu MB) exceeded; drawing some translucent "
                                "objects directly (overlaps may darken)\n", r->budget >> 20);
                r->warned_budget = true;
            }
        }
    }
    for (int li = m->nlayers - 1; li >= 0; li--) {
        const ani_layer_rect *lr = &r->rects[li];
        if (lr->empty || lr->tex < 0) continue;
        const ani_rt_scratch *sc = &r->scratches[lr->scratch];
        sg_begin_pass(&(sg_pass){
            .action = {
                .colors[0] = { .load_action = SG_LOADACTION_CLEAR, .store_action = SG_STOREACTION_DONTCARE,
                               .clear_value = { 0, 0, 0, 0 } },
                .depth = { .load_action = SG_LOADACTION_CLEAR, .store_action = SG_STOREACTION_DONTCARE, .clear_value = 1 },
                .stencil = { .load_action = SG_LOADACTION_CLEAR, .store_action = SG_STOREACTION_DONTCARE, .clear_value = 0 },
            },
            .attachments = {
                .colors[0] = sc->color_view,
                .resolves[0] = r->resolves[lr->tex].resolve_view,
                .depth_stencil = sc->ds_view,
            },
            .label = "anown-layer",
        });
        const target_ctx t = { .index = li, .ox = lr->x, .oy = lr->y, .w = lr->w, .h = lr->h };
        draw_target(r, &t, li, 1.0f, false);
        sg_end_pass();
        r->layers_rendered++;
    }
    r->prof_end = uptime_s();
    return true;
}

void ani_renderer_draw(ani_renderer *r) {
    const target_ctx t = { .index = ANI_TARGET_MAIN, .ox = 0, .oy = 0, .w = r->tw, .h = r->th };
    draw_target(r, &t, ANI_TARGET_MAIN, 1.0f, false);
}
