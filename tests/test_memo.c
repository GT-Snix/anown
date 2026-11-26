// Memoized tessellation (src/tess.c, docs/layers.md "Memoized tessellation"):
// the cache must never change the output. For every frame, the mesh (vertex
// bytes and commands) and the rendered pixels with the cache on must equal the
// cache-off render of that frame, whatever order the frames are rendered in:
// forward, reverse, shuffled, and with a budget so small that entries are
// evicted all the time. The scenes cover what the key has to capture: boil
// steps, motion, draw-on, color tracks, riso and shadows (chaos ramp),
// patterns, translucent paints in shared layers, a fading group, text with
// draw-on and a per-letter stagger, a code block with a highlight.
#include "anown.h"
#include "check.h"
#include "offscreen_internal.h"
#include "render_internal.h"
#include "scene_internal.h"

#include <stdlib.h>
#include <string.h>

enum { W = 640, H = 360 };

static ani_offscreen g_off;
static ani_renderer g_r;
static unsigned char *g_px;

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}

typedef struct frame_hash {
    uint64_t mesh, pixels;
} frame_hash;

static frame_hash render(an_scene *s, int64_t frame) {
    void *pool = ani_pool_push();
    CHECK(ani_renderer_prepare(&g_r, s, frame, W, H));
    ani_offscreen_begin(&g_off, AN_BLACK);
    ani_renderer_draw(&g_r);
    sg_end_pass();
    sg_commit();
    ani_offscreen_readback(&g_off, g_px);
    ani_pool_pop(pool);
    const ani_mesh *m = &g_r.mesh;
    frame_hash h = { 1469598103934665603ull, 1469598103934665603ull };
    h.mesh = fnv(h.mesh, m->v, sizeof(ani_vertex) * (size_t)m->count);
    for (int i = 0; i < m->ncmds; i++) {
        const ani_cmd *c = &m->cmds[i];
        const int fields[6] = { (int)c->kind, c->target, (int)c->paint, c->first, c->count, c->layer };
        h.mesh = fnv(h.mesh, fields, sizeof fields);
    }
    h.mesh = fnv(h.mesh, &m->nlayers, sizeof m->nlayers);
    h.pixels = fnv(h.pixels, g_px, (size_t)W * H * 4);
    return h;
}

// variant 1 has other rect sizes (immutable geometry, which isn't in the key:
// the scene serial and object id stand for it) and the same ids, transforms and
// colors, so a cache that confused the two scenes would replay the old rects
static an_scene *scene_shapes(int variant) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .width = W, .height = H, .fps = 30, .seed = 21, .style = { .shadow = true } });
    const an_palette *p = &AN_PALETTE_RIOT;
    an_set_chaos(s, 0.0f);
    an_obj *g = an_group(s, NULL);
    // a translucent rect that blinks: while it's visible, the group layer's
    // stencil is dirty when rect 2 (translucent, later) draws, so rect 2 gets a
    // stencil clear; while it's hidden, not. Same rect 2 key otherwise.
    an_obj *blink = an_rect(s, &(an_rect_opts){ .parent = g, .pos = { 60, 220 }, .size = { 40, 40 }, .fill = { 0, 0, 0, 0.5f } });
    for (int k = 0; k < 6; k++) {
        an_wait(s, 0.3f);
        an_obj_set_opacity(blink, k % 2 ? 1.0f : 0.0f);
    }
    s->cursor = 0; // the rest starts at 0 (the cursor is plain script state)
    for (int i = 0; i < 8; i++) {
        an_color f = i % 2 ? p->a : p->b;
        if (i % 3 == 2) f.a = 0.6f; // translucent fill: isolated, write-once marks
        an_obj *o = an_rect(s, &(an_rect_opts){ .parent = i < 4 ? g : NULL, .pos = { 60.0f + 70.0f * (float)i, 120 },
                                                .size = { variant ? 50.0f : 80.0f, 60 }, .fill = f, .stroke = p->ink, .stroke_width = 5,
                                                .pattern = i == 1 ? (an_pattern){ .kind = AN_PATTERN_HALFTONE, .size = 10 } : (an_pattern){ 0 },
                                                .paper_under = i == 5 });
        if (i == 3) an_play_async(s, an_move_by(o, (an_vec2){ 0, 120 }, &(an_anim_opts){ .duration = 1.5f }));
        if (i == 6) an_play_async(s, an_fill_to(o, p->c, &(an_anim_opts){ .duration = 1.0f }));
    }
    an_obj *st = an_stroke(s, &(an_stroke_opts){ .segs = (an_seg[]){ AN_MOVE(40, 300), AN_CUBIC(200, 200, 400, 380, 600, 280) },
                                                 .seg_count = 2, .stroke = p->ink, .stroke_width = 8, .undrawn = true,
                                                 .brush = { .taper_in = 20, .taper_out = 40 } });
    an_play_async(s, an_draw(st, &(an_anim_opts){ .duration = 1.2f }));
    an_play_async(s, an_scene_chaos_to(s, 1.0f, &(an_anim_opts){ .duration = 0.6f }));
    an_play_async(s, an_fade_to(g, 0.5f, &(an_anim_opts){ .duration = 1.0f }));
    // the camera pans (a pure translation: the per-draw fast path)
    an_play_async(s, an_camera_move_to(s, (an_vec2){ 360, 150 }, &(an_anim_opts){ .duration = 1.6f }));
    // and shakes twice (overlapping events), translation only: still the fast path
    an_wait(s, 0.3f);
    an_play_async(s, an_shake(s, 10.0f, &(an_anim_opts){ .duration = 0.6f }));
    an_wait(s, 0.4f);
    an_play_async(s, an_shake(s, 6.0f, &(an_anim_opts){ .duration = 0.5f }));
    s->cursor = 0;
    an_wait(s, 2.0f);
    return s;
}

static an_scene *scene_text(void) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .width = W, .height = H, .fps = 30, .seed = 22, .style = { .shadow = true } });
    const an_palette *p = &AN_PALETTE_RIOT;
    an_obj *t = an_text(s, &(an_text_opts){ .text = "MEMO", .pos = { 320, 110 }, .size = 90, .align = AN_ALIGN_CENTER,
                                             .fill = p->a, .stroke = p->ink, .stroke_width = 5, .undrawn = true });
    an_obj *sub = an_text(s, &(an_text_opts){ .text = "cache", .pos = { 320, 170 }, .size = 40, .align = AN_ALIGN_CENTER,
                                               .fill = p->b, .letters_hidden = true });
    // a typewriter: letters appear while their style stays the same
    an_obj *tw = an_text(s, &(an_text_opts){ .text = "typed", .pos = { 500, 320 }, .size = 40, .fill = p->ink, .undrawn = true });
    an_play_async(s, an_draw(tw, &(an_anim_opts){ .duration = 1.5f, .ease = AN_EASE_LINEAR }));
    an_obj *c = an_code(s, &(an_code_opts){ .pos = { 140, 200 }, .code = "x = 1;\ny = x + 2;", .line_numbers = true });
    an_play_async(s, an_draw(t, &(an_anim_opts){ .duration = 1.0f }));
    an_anim fades[5];
    for (int i = 0; i < 5; i++) fades[i] = an_fade_to(an_text_glyph(sub, i), 1.0f, &(an_anim_opts){ .duration = 0.3f });
    an_stagger(s, 5, fades, 0.1f);
    an_play(s, an_fade_to(an_code_line(c, 2), 1.0f, &(an_anim_opts){ .duration = 0.3f }));
    an_play(s, an_fill_to(t, p->c, &(an_anim_opts){ .duration = 0.5f }));
    // a zoom out and back to exactly 1: frames switch between the camera in the
    // world transforms and the fast path
    an_play(s, an_camera_zoom_to(s, 1.3f, &(an_anim_opts){ .duration = 0.3f }));
    an_play(s, an_camera_zoom_to(s, 1.0f, &(an_anim_opts){ .duration = 0.3f }));
    an_wait(s, 0.3f);
    return s;
}

// a 3D space (docs/space.md) with the camera still: a filled globe spinning in a
// frame (only the frame's composed map and the eye in the sphere's space change;
// the root view doesn't), its far wire at 0.4, an egg that stretches in another
// frame, then holds (its keys repeat), and the grid and axes
static an_scene *scene_space(void) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .width = W, .height = H, .fps = 30, .seed = 23 });
    an_set_chaos(s, 0.0f);
    an_obj *sp = an_space(s, &(an_space_opts){ .axes = true, .unit = 40, .orbit = { 0.4f, 1.0f, 12 } });
    an_obj *globe = an_space_frame(sp, &(an_frame_opts){ .origin = { -2, 0, 1 }, .spin_axis = { 0.3f, 0, 1 } });
    an_space_sphere(globe, &(an_sphere_opts){ .radius = 1.2f, .fill = AN_HEX(0xF3EDE2), .back = 0.4f });
    an_space_line(globe, (an_vec3){ 0, 0, 0 }, (an_vec3){ 0, 0, 1 }, &(an_line_opts){ .stroke = AN_HEX(0xFF6A00) });
    an_obj *egg = an_space_frame(sp, &(an_frame_opts){ .origin = { 2, 0, 1 } });
    an_space_sphere(egg, &(an_sphere_opts){ .radius = 0.6f });
    an_play_async(s, an_frame_spin_to(globe, 3.0f, &(an_anim_opts){ .duration = 2, .ease = AN_EASE_LINEAR }));
    an_play(s, an_space_matrix_to(egg, an_mat3_scale((an_vec3){ 2, 1.5f, 1 }), &(an_anim_opts){ .duration = 0.6f }));
    an_wait(s, 1.6f);
    return s;
}

static void check_scene(an_scene *s, const char *name) {
    const int64_t L = an_scene_length(s);
    frame_hash *ref = calloc((size_t)L + 1, sizeof *ref);
    g_r.mesh.memo_off = true;
    for (int64_t f = 0; f <= L; f++) ref[f] = render(s, f);
    g_r.mesh.memo_off = false;
    // shuffled order: Fisher-Yates with the scene-independent stream
    int64_t *order = malloc(sizeof *order * (size_t)(L + 1));
    for (int64_t i = 0; i <= L; i++) order[i] = i;
    an_rng rng = an_rng_make(99, 1);
    for (int64_t i = L; i > 0; i--) {
        const int64_t j = an_rng_below(&rng, (uint32_t)(i + 1));
        const int64_t t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    static const char *names[4] = { "forward", "reverse", "shuffled", "tiny budget" };
    for (int pass = 0; pass < 4; pass++) {
        g_r.mesh.memo_budget = pass == 3 ? 64 * 1024 : 0;
        int bad = 0, hits = 0, misses = 0;
        for (int64_t i = 0; i <= L; i++) {
            const int64_t f = pass == 0 || pass == 3 ? i : pass == 1 ? L - i : order[i];
            const frame_hash h = render(s, f);
            bad += h.mesh != ref[f].mesh || h.pixels != ref[f].pixels;
            hits += g_r.mesh.memo_hits;
            misses += g_r.mesh.memo_misses;
        }
        if (bad) fprintf(stderr, "  %s, %s: %d of %lld frames differ\n", name, names[pass], bad, (long long)L + 1);
        CHECK(bad == 0);
        printf("  %s, %s: %lld frames identical; cache %d hits, %d misses\n", name, names[pass], (long long)L + 1, hits, misses);
        if (pass == 0) CHECK(hits > 0); // the cache is actually used
    }
    g_r.mesh.memo_budget = 0;
    free(order);
    free(ref);
}

int main(void) {
    if (!ani_gfx_setup_headless()) {
        fprintf(stderr, "test_memo: headless Metal setup failed\n");
        return 1;
    }
    CHECK(ani_offscreen_make_ds(&g_off, W, H, 4));
    CHECK(ani_renderer_init(&g_r, SG_PIXELFORMAT_RGBA8, SG_PIXELFORMAT_DEPTH_STENCIL, 4));
    g_px = malloc(ani_offscreen_frame_bytes(&g_off));

    an_scene *a = scene_shapes(0);
    check_scene(a, "shapes");
    an_scene *b = scene_text();
    check_scene(b, "text");
    an_scene *sp = scene_space();
    check_scene(sp, "space (spinning frame, sphere)");
    an_scene_destroy(sp);
    // a new scene, possibly at the old one's address, with the same ids and
    // other geometry: the cache must not replay the old scene's entries (it compares a
    // per-scene serial). Without a cache-off reference pass in between: the
    // renderer goes straight from the old scene to the new one.
    // fill the cache with the old scene, ending at frame 0: the new scene's
    // frame 0 has the same ids and keys, only other geometry
    // (frame 0 twice: an entry is stored once its key repeats)
    for (int64_t f = an_scene_length(a); f >= 0; f--) render(a, f);
    render(a, 0);
    an_scene_destroy(a);
    a = scene_shapes(1);
    check_scene(a, "shapes, rebuilt with other geometry");
    an_scene_destroy(a);
    an_scene_destroy(b);

    free(g_px);
    ani_renderer_shutdown(&g_r);
    ani_offscreen_destroy(&g_off);
    ani_gfx_shutdown();
    return check_done("test_memo");
}
