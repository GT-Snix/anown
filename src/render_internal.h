// Internal: renders a scene. The same code serves the preview window and the
// offscreen export; only the main pass differs. docs/layers.md has the design.
//
//   ani_renderer_prepare(r, s, frame, tw, th);   // outside any pass: tessellate,
//                                                // render + resolve isolated layers
//   sg_begin_pass(main ...);
//   ani_renderer_draw(r);                         // direct paints + composites
//   sg_end_pass(); sg_commit();
#ifndef ANOWN_RENDER_INTERNAL_H
#define ANOWN_RENDER_INTERNAL_H

#include "sokol_gfx.h"
#include "tess_internal.h"

// Layers always render at 4x MSAA into RGBA8 + depth-stencil.
#define ANI_LAYER_SAMPLES 4
// Max memory for the layer pool (MSAA scratch + resolve textures).
#define ANI_LAYER_BUDGET ((size_t)512 * 1024 * 1024)
// Layer size classes: width and height rounded up to a multiple of this.
#define ANI_LAYER_CLASS 128

typedef struct ani_rt_scratch {
    int w, h;
    sg_image color, ds;
    sg_view color_view, ds_view;
} ani_rt_scratch;

typedef struct ani_rt_resolve {
    int w, h;
    sg_image img;
    sg_view resolve_view, tex_view;
    bool in_use;
} ani_rt_resolve;

// A layer's rectangle for this frame, in target pixels.
typedef struct ani_layer_rect {
    int x, y, w, h;
    bool empty;
    int tex;      // index into resolves, or -1: no memory, replay directly (fallback)
    int scratch;  // index into scratches
} ani_layer_rect;

typedef struct ani_renderer {
    // main target
    sg_pipeline pip_main, pip_comp_main, pip_paper;
    // main-target stencil pipelines (only when the main pass has a depth-stencil buffer)
    sg_pipeline pip_main_shadow, pip_main_sil, pip_main_silclr;
    bool main_stencil;
    // layer targets: one per paint (index ani_paint), plus composite
    sg_pipeline pip_layer[ANI_PAINT_COUNT], pip_comp_layer;
    sg_shader shape_shd, comp_shd, paper_shd;
    sg_sampler smp;

    sg_buffer vbuf;
    size_t vbuf_bytes;
    ani_mesh mesh;

    // pool (GPU resources, kept across frames; not scene state)
    ani_rt_scratch scratches[64];
    int nscratch;
    ani_rt_resolve *resolves;
    int nresolve, resolve_cap;
    size_t pool_bytes;
    bool warned_budget;
    size_t budget; // ANI_LAYER_BUDGET; tests may lower it

    // this frame
    ani_layer_rect *rects;
    int rect_cap;
    int tw, th;              // target size, pixels
    float k, vx, vy, vw, vh; // scene -> target: px = v + scene * k (main viewport)
    float sw, sh;            // scene size
    int layers_rendered, layers_fallback; // stats for the frame
    // profiling (spike_frame_bench): when prepare started, finished tessellating,
    // finished the upload, and returned. Seconds on CLOCK_UPTIME_RAW (mach
    // absolute time). Measurement only; nothing reads them to render.
    double prof_start, prof_tess, prof_upload, prof_end;
    // paper background for this frame (docs/layers.md, 5B)
    bool paper;              // draw the background with the paper shader
    float paper_color[4], grain, vignette; // grain and vignette already x scene chaos
    uint32_t seed_lo, seed_hi, grain_step;
    // capacity reserved for this scene (estimate, once per scene)
    const an_scene *reserved_for;
    uint32_t reserved_count;
} ani_renderer;

// Main-pass formats and sample count (the preview swapchain has a depth buffer;
// the export target doesn't). Layer formats are fixed (see above).
bool ani_renderer_init(ani_renderer *r, sg_pixel_format color_format,
                       sg_pixel_format depth_format, int sample_count);
void ani_renderer_shutdown(ani_renderer *r);

// Outside any pass: tessellate the scene at `frame`, upload vertices, and render
// and resolve every isolated layer (deepest first). The main target is
// tw x th pixels; the scene is letterboxed into it. False on allocation failure.
bool ani_renderer_prepare(ani_renderer *r, const an_scene *s, int64_t frame, int tw, int th);

// Inside the main pass: draw the main command list (direct paints and layer
// composites). The area outside the letterbox is left as the pass cleared it.
void ani_renderer_draw(ani_renderer *r);

#endif
