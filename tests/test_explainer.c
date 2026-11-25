// Explainer objects (docs/explainer.md): the arc track, the array / bar
// visualizer. Every expected value is worked out by hand in the comments.
//
// paper #F3EDE2 = (243, 237, 226); charcoal #16161A = (22, 22, 26)
#include "anown.h"
#include "check.h"
#include "offscreen_internal.h"
#include "render_internal.h"
#include "scene_internal.h"
#include "timeline_internal.h"

#include <stdlib.h>
#include <string.h>

enum { W = 1920, H = 1080 };

static ani_offscreen g_off;
static ani_renderer g_r;
static unsigned char *g_px;

static void render_with(ani_renderer *r, an_scene *s, int64_t frame) {
    void *pool = ani_pool_push();
    CHECK(ani_renderer_prepare(r, s, frame, W, H));
    ani_offscreen_begin(&g_off, AN_BLACK);
    ani_renderer_draw(r);
    sg_end_pass();
    sg_commit();
    ani_offscreen_readback(&g_off, g_px);
    ani_pool_pop(pool);
}
static void render(an_scene *s, int64_t frame) { render_with(&g_r, s, frame); }

static const unsigned char *at(int x, int y) { return g_px + ((size_t)y * W + (size_t)x) * 4; }
static bool is(int x, int y, int r, int g, int b) {
    const unsigned char *p = at(x, y);
    const bool ok = p[0] == r && p[1] == g && p[2] == b;
    if (!ok) fprintf(stderr, "  (%d,%d) = %d %d %d, want %d %d %d\n", x, y, p[0], p[1], p[2], r, g, b);
    return ok;
}
static bool near(int x, int y, double r, double g, double b, double tol) {
    const unsigned char *p = at(x, y);
    const bool ok = fabs(p[0] - r) <= tol && fabs(p[1] - g) <= tol && fabs(p[2] - b) <= tol;
    if (!ok) fprintf(stderr, "  (%d,%d) = %d %d %d, want %.1f %.1f %.1f\n", x, y, p[0], p[1], p[2], r, g, b);
    return ok;
}
#define PAPER(x, y) is(x, y, 243, 237, 226)

static an_scene *scene(void) {
    return an_scene_create(&(an_scene_desc){ .width = W, .height = H, .fps = 60, .seed = 6,
                                             .style = { .grain_off = true, .vignette_off = true, .boil_off = true } });
}

static an_vec2 pos_at(an_obj *o, an_scene *s, int64_t f) {
    const ani_value v = ani_sample(ani_obj_prop(o, AN_PROP_POS), ani_frame_time(s, f));
    return (an_vec2){ (float)v.v[0], (float)v.v[1] };
}

// ---- the arc track ---------------------------------------------------------------------------

// From (100, 300) to (500, 300), height 120, 1 s in-out at 60 fps. The chord
// points right, so the normal is (0, -1); c = 4 x 120 / 3 = 160:
// P1 = (100, 140), P2 = (500, 140). At frame 30 the eased progress is exactly
// 0.5: B(0.5) = (P0 + 3 P1 + 3 P2 + P3) / 8 = (300, (300 + 420 + 420 + 300) / 8) =
// (300, 180), i.e. 120 up. Frame 60 and later: exactly (500, 300).
// Height -50 bulges down: y 350 at the middle. A vertical chord (100, 300) ->
// (100, 500), height 40: the normal is -x, so x = 60 at the middle.
static void test_arc_track(void) {
    an_scene *s = scene();
    an_obj *a = an_rect(s, &(an_rect_opts){ .pos = { 100, 300 } });
    an_obj *b = an_rect(s, &(an_rect_opts){ .pos = { 100, 300 } });
    an_obj *c = an_rect(s, &(an_rect_opts){ .pos = { 100, 300 } });
    an_play_async(s, an_move_arc(a, (an_vec2){ 500, 300 }, 120, &(an_anim_opts){ .duration = 1 }));
    an_play_async(s, an_move_arc(b, (an_vec2){ 500, 300 }, -50, &(an_anim_opts){ .duration = 1 }));
    an_play(s, an_move_arc(c, (an_vec2){ 100, 500 }, 40, &(an_anim_opts){ .duration = 1 }));
    an_wait(s, 0.2f);
    CHECK_NEAR(pos_at(a, s, 30).x, 300, 1e-3);
    CHECK_NEAR(pos_at(a, s, 30).y, 180, 1e-3);
    CHECK_NEAR(pos_at(b, s, 30).y, 350, 1e-3);
    CHECK_NEAR(pos_at(c, s, 30).x, 60, 1e-3);
    CHECK_NEAR(pos_at(c, s, 30).y, 400, 1e-3);
    for (int64_t f = 60; f <= 72; f++) CHECK(pos_at(a, s, f).x == 500.0f && pos_at(a, s, f).y == 300.0f);
    an_scene_destroy(s);

    // take-over with a linear ease: a linear move of +200 px/s in x from 0 to 1 s;
    // at 0.5 s an arc (linear, 1 s, height 100) to (600, 300) starts. Its start
    // tangent is 3 (P1 - P0) = 3 n c + v = (200, -400) px/s: the x velocity carries
    // over (200 before and after); the lift adds the y component.
    s = scene();
    an_obj *o = an_rect(s, &(an_rect_opts){ .pos = { 100, 300 } });
    an_play_async(s, an_move_by(o, (an_vec2){ 200, 0 }, &(an_anim_opts){ .duration = 1, .ease = AN_EASE_LINEAR }));
    an_wait(s, 0.5f);
    an_play(s, an_move_arc(o, (an_vec2){ 600, 300 }, 100, &(an_anim_opts){ .duration = 1, .ease = AN_EASE_LINEAR }));
    ani_value v_before, v_after;
    const ani_value p_before = ani_sample_vel(ani_obj_prop(o, AN_PROP_POS), ani_frame_time(s, 29), &v_before);
    const ani_value p_at = ani_sample_vel(ani_obj_prop(o, AN_PROP_POS), ani_frame_time(s, 30), &v_after);
    CHECK_NEAR(v_before.v[0], 200.0, 1e-6);
    CHECK_NEAR(v_after.v[0], 200.0, 1e-6);
    CHECK_NEAR(v_after.v[1], -400.0, 1e-6);
    CHECK_NEAR(p_at.v[0], 200.0, 1e-6); // 100 + 200 x 0.5
    CHECK_NEAR(p_before.v[0], 100.0 + 200.0 * 29.0 / 60.0, 1e-6);
    an_scene_destroy(s);
}

// ---- the array ---------------------------------------------------------------------------------

static an_obj *array5(an_scene *s, an_vec2 pos, const an_pattern *pat) {
    static const int v[5] = { 4, 9, 2, 7, 5 };
    return an_array(s, &(an_array_opts){ .pos = pos, .values = v, .count = 5, .index_labels = true,
                                         .pattern = pat ? *pat : (an_pattern){ 0 } });
}

// n = 5, cell 96, gap 0: slot centers -192, -96, 0, 96, 192. Each cell's parent is
// an element group, whose parent is the array; index labels are the array's own
// children at their slot's x. Bars for values {1, 2, 4}, height 320 for the max:
// 80, 160, 320.
static void test_layout(void) {
    an_scene *s = scene();
    an_obj *a = array5(s, (an_vec2){ 960, 900 }, NULL);
    CHECK(an_obj_kind(a) == AN_KIND_ARRAY);
    const float want[5] = { -192, -96, 0, 96, 192 };
    int labels = 0;
    for (int k = 0; k < 5; k++) {
        CHECK(an_array_slot_pos(a, k).x == want[k] && an_array_slot_pos(a, k).y == 0.0f);
        an_obj *e = an_array_item(a, k), *c = an_array_cell(a, k);
        CHECK(an_obj_parent(c) == e && an_obj_parent(e) == a && e->pos.x == want[k]);
    }
    for (an_obj *c = a->first_child; c; c = c->next_sibling) {
        if (c->kind != AN_KIND_TEXT) continue;
        const int k = c->u.text.str[0] - '0';
        CHECK(c->pos.x == want[k]);
        labels++;
    }
    CHECK(labels == 5);
    static const int bv[3] = { 1, 2, 4 };
    an_obj *b = an_array(s, &(an_array_opts){ .values = bv, .count = 3, .style = AN_ARRAY_BARS });
    CHECK(an_array_cell(b, 0)->u.rect.size.y == 80.0f && an_array_cell(b, 1)->u.rect.size.y == 160.0f &&
          an_array_cell(b, 2)->u.rect.size.y == 320.0f);
    CHECK(an_array_cell(b, 2)->pos.y == -160.0f); // bottoms on the baseline
    an_scene_destroy(s);
}

typedef struct box { float x, y; } box; // a cell-sized box center (96 x 96)
static float overlap_w(box a, box b) { // overlap width if the boxes intersect, else 0
    const float dx = fabsf(a.x - b.x), dy = fabsf(a.y - b.y);
    return (dx < 96.0f && dy < 96.0f) ? 96.0f - dx : 0.0f;
}

// Swap 1 <-> 3 (d = 192, one element between), lifted arcs: h_mid = 96, so
// H_B = max(1.5 x 96, 0.25 x 192) = 144; nothing travels until the eased
// progress reaches lift = (1 - sqrt(1 - 96/144)) / 2 = 0.211325 (B is then 96 up,
// clear of the element in slot 2). They overlap horizontally from the travel
// phase k1 with smoothstep S(k1) = (1 - 96/192) / 2 = 0.25: k1 = 0.326352, i.e.
// u1 = 0.211325 + k1 (1 - 2 x 0.211325) = 0.399743, 4 u1 (1 - u1) = 0.959794, so
// H_A = 144 + 1.05 x 96 / 0.959794 = 249.02. 0.9 s at 60 fps: the eased progress
// is 0.5 at frame 27: A at (0, -249.02), B at (0, -144). End (frame 54): A exactly
// at slot 3 (96, 0), B at slot 1 (-96, 0). Boxes (96 x 96 at the positions):
// nothing ever overlaps, and nothing goes below the row (y <= 0).
// Adjacent swap 0 <-> 1 after it: H = max(144, 0.35 x 96) = 144; A at -144 at
// its midpoint (frame 81); B stays on the row; no overlap.
static void test_swap(void) {
    an_scene *s = scene();
    an_obj *a = array5(s, (an_vec2){ 960, 900 }, NULL);
    an_obj *e1 = an_array_item(a, 1), *e2 = an_array_item(a, 2), *e3 = an_array_item(a, 3);
    an_play(s, an_array_swap(a, 1, 3, NULL));
    CHECK(an_array_item(a, 1) == e3 && an_array_item(a, 3) == e1 && an_array_item(a, 2) == e2);
    an_obj *e0 = an_array_item(a, 0);
    const int64_t t1 = (int64_t)(an_scene_time(s) * 60.0 + 0.5);
    an_play(s, an_array_swap(a, 0, 1, NULL)); // e0 arcs over, e3 slides under
    an_wait(s, 0.5f);
    CHECK(t1 == 54);
    CHECK_NEAR(pos_at(e1, s, 27).x, 0, 1e-3);
    CHECK_NEAR(pos_at(e1, s, 27).y, -249.02, 0.01);
    CHECK_NEAR(pos_at(e3, s, 27).y, -144, 1e-3);
    CHECK(pos_at(e1, s, 54).x == 96.0f && pos_at(e1, s, 54).y == 0.0f);
    CHECK(pos_at(e3, s, 54).x == -96.0f && pos_at(e3, s, 54).y == 0.0f);
    float ab = 0, am = 0, below = 0, adj = 0;
    for (int64_t f = 0; f <= 54; f++) {
        const an_vec2 pa = pos_at(e1, s, f), pb = pos_at(e3, s, f), pm = pos_at(e2, s, f);
        ab = fmaxf(ab, overlap_w((box){ pa.x, pa.y }, (box){ pb.x, pb.y }));
        am = fmaxf(am, fmaxf(overlap_w((box){ pa.x, pa.y }, (box){ pm.x, pm.y }), overlap_w((box){ pb.x, pb.y }, (box){ pm.x, pm.y })));
        below = fmaxf(below, fmaxf(pa.y, pb.y));
    }
    if (!(ab == 0.0f && am == 0.0f && below <= 0.0f)) fprintf(stderr, "  ab %.3f am %.3f below %.3f\n", ab, am, below);
    CHECK(ab == 0.0f && am == 0.0f && below <= 0.0f);
    // the adjacent swap: frames 54..108; midpoint 81
    CHECK_NEAR(pos_at(e0, s, 81).y, -144, 1e-3);
    for (int64_t f = 54; f <= 108; f++) {
        const an_vec2 pa = pos_at(e0, s, f), pb = pos_at(e3, s, f);
        adj = fmaxf(adj, overlap_w((box){ pa.x - 0, pa.y }, (box){ pb.x, pb.y }));
        CHECK(pb.y == 0.0f && pa.y <= 0.0f);
    }
    CHECK(adj == 0.0f);
    // seek order: the same positions forward, reverse and shuffled
    const int64_t L = an_scene_length(s);
    float fwd[200], rev[200];
    for (int64_t f = 0; f <= L; f++) fwd[f] = pos_at(e1, s, f).y + pos_at(e3, s, f).x;
    for (int64_t f = L; f >= 0; f--) rev[f] = pos_at(e1, s, f).y + pos_at(e3, s, f).x;
    CHECK(memcmp(fwd, rev, sizeof(float) * (size_t)(L + 1)) == 0);
    an_scene_destroy(s);
}

// Seeking straight into the middle of a swap (a fresh renderer, one frame) gives
// the same pixels as rendering every frame up to it.
static void test_seek_middle(void) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .width = W, .height = H, .fps = 60, .seed = 6 }); // full look
    an_obj *a = array5(s, (an_vec2){ 960, 900 }, &(an_pattern){ .kind = AN_PATTERN_HALFTONE, .size = 12 });
    an_play(s, an_array_swap(a, 1, 3, NULL));
    const size_t bytes = (size_t)W * H * 4;
    unsigned char *seq = malloc(bytes);
    for (int64_t f = 0; f <= 27; f++) render(s, f);
    memcpy(seq, g_px, bytes);
    ani_renderer fresh;
    CHECK(ani_renderer_init(&fresh, SG_PIXELFORMAT_RGBA8, SG_PIXELFORMAT_DEPTH_STENCIL, 4));
    render_with(&fresh, s, 27);
    CHECK(memcmp(seq, g_px, bytes) == 0);
    ani_renderer_shutdown(&fresh);
    free(seq);
    an_scene_destroy(s);
}

// Highlight: slot 2's cell to magenta (exactly at the end); a later swap 2 <-> 4
// carries it: the cell now in slot 4 is magenta; unhighlight slot 4 -> exactly the
// array's fill (riot's accent a, #00C2FF).
static void test_highlight(void) {
    an_scene *s = scene();
    an_obj *a = array5(s, (an_vec2){ 960, 900 }, NULL);
    const an_color mag = AN_HEX(0xFF2E88);
    an_play(s, an_array_highlight(a, 2, mag, &(an_anim_opts){ .duration = 0.3f }));
    an_play(s, an_array_swap(a, 2, 4, NULL));
    an_obj *c4 = an_array_cell(a, 4);
    an_seek(s, (int64_t)(an_scene_time(s) * 60.0 + 0.5));
    an_obj_state st = an_obj_get(c4);
    CHECK(st.fill.r == mag.r && st.fill.g == mag.g && st.fill.b == mag.b);
    an_play(s, an_array_unhighlight(a, 4, &(an_anim_opts){ .duration = 0.3f }));
    an_seek(s, an_scene_length(s));
    st = an_obj_get(c4);
    const an_color cyan = AN_HEX(0x00C2FF);
    CHECK(st.fill.r == cyan.r && st.fill.g == cyan.g && st.fill.b == cyan.b);
    an_scene_destroy(s);
}

// Rule 13: the array fades as one group, one layer. Chaos 0. Slots 0 and 1 share
// the edge at x = 960 - 144 = 816 (array at 960: slot 0 is -192 +- 48); the ink
// (charcoal, width 5) covers it from both cells. At opacity 0.995 the pixel is
// charcoal x 0.995 + paper x 0.005 = (23.1, 23.1, 27.0), with no pale seam.
static void test_fade_one_layer(void) {
    an_scene *s = scene();
    an_set_chaos(s, 0.0f);
    an_obj *a = array5(s, (an_vec2){ 960, 900 }, NULL);
    an_obj_set_opacity(a, 0.995f);
    render(s, 0);
    CHECK(g_r.layers_rendered == 1);
    CHECK(near(816, 870, 23.1, 23.1, 27.0, 1.0) && near(815, 870, 23.1, 23.1, 27.0, 1.0));
    an_scene_destroy(s);
}

// paper_under defaults: halftone cells (size 12, density 0.5) get it, solid cells
// don't. Over a charcoal rect, the dot gap at a cell corner of the pattern grid
// shows paper. Slot 2's cell is centered at (960, 900); the pattern is anchored
// in the cell's space, dots at (6 + 12i, 6 + 12j) from its center, gaps at the
// grid corners: (960 + 36, 900 + 36) = (996, 936), away from the value label.
static void test_paper_under_default(void) {
    an_scene *s = scene();
    an_set_chaos(s, 0.0f);
    an_rect(s, &(an_rect_opts){ .pos = { 960, 900 }, .size = { 600, 200 }, .fill = AN_HEX(0x16161A) });
    an_obj *a = array5(s, (an_vec2){ 960, 900 }, &(an_pattern){ .kind = AN_PATTERN_HALFTONE, .size = 12, .density = 0.5f });
    an_obj *b = array5(s, (an_vec2){ 960, 300 }, NULL);
    CHECK(an_array_cell(a, 2)->paper_under && !an_array_cell(b, 2)->paper_under);
    render(s, 0);
    CHECK(PAPER(996, 936));
    an_scene_destroy(s);
}

// ---- the code marker --------------------------------------------------------------------------

// an_code at pos (40, 40), mono 30, no line numbers: code from x 18 (block),
// width 1 column = 18; baselines 51 + 40 (n - 1) in the block, so line 3 at 131 and
// line 5 at 211 (+40 on screen: 171, 251). The marker's bar covers the line box,
// baseline - 30.6 .. baseline + 9, and x 18 - 9 .. 18 + 18 + 9 = 9..45 (49..85 on
// screen); column 82 is right of the letters (x 58..76). Line 5: rows 220.4..260:
// 221..259 magenta (riot's accent b, #FF2E88 = (255, 46, 136)), row 220 half (the
// samples at +0.625 and +0.875 are inside: (249, 141.5, 181)), 260 paper.
// Line 3: rows 140.4..180. Timeline: goto 3 at 0 (a 0.25 s fade: frames 0..15),
// then goto 5 (a 0.35 s slide: frames 15..36).
static void test_code_marker(void) {
    an_scene *s = scene();
    an_set_chaos(s, 0.0f);
    an_obj *c = an_code(s, &(an_code_opts){ .pos = { 40, 40 }, .code = "a\nb\nc\nd\ne\nf" });
    an_code_marker(c, NULL);
    render(s, 0);
    CHECK(PAPER(82, 160) && PAPER(82, 240)); // hidden before its first goto
    an_play(s, an_code_goto(c, 3, NULL));
    an_play(s, an_code_goto(c, 5, NULL));
    an_wait(s, 0.2f);
    CHECK(an_code_current_line(c) == 5);
    // frame 8: fading in AT line 3 (the first goto doesn't slide): tau = 8/15,
    // in-out alpha = 1 - (2 - 2 tau)^3 / 2 = 0.5935: magenta x 0.5935 + paper x 0.4065
    render(s, 8);
    CHECK(near(82, 160, 250.1, 123.6, 172.6, 1.5));
    render(s, 15);
    CHECK(is(82, 160, 255, 46, 136) && PAPER(82, 240));
    render(s, 36);
    CHECK(is(82, 221, 255, 46, 136) && is(82, 240, 255, 46, 136) && is(82, 259, 255, 46, 136));
    CHECK(near(82, 220, 249, 141.5, 181, 1) && PAPER(82, 260) && PAPER(82, 160));
    an_scene_destroy(s);
}

// ---- callouts and pointers ------------------------------------------------------------------

static an_vec2 path_pt(const an_obj *stroke, int sub, int last) {
    const ani_subpath *sp = &stroke->path->subs[sub];
    return stroke->path->pts[sp->first + (last ? sp->count - 1 : 0)];
}

// Label "ab" in the mono at 34 px at (300, 300): 2 x 600 x 0.034 = 40.8 wide, so
// the arrow leaves the box at half the width + 8 = 28.4 (the group's origin is the
// label's center). Target (500, 300), i.e. (200, 0) locally, gap 10:
//   bend 0: from (28.4, 0) to (190, 0)
//   bend 0.5: chord 28.4 -> 200 (171.6 long, middle 114.2); control pushed up by
//     0.5 x 171.6 / 2 = 42.9: (114.2, -42.9). End tangent (85.8, 42.9) / 95.927 =
//     (0.89443, 0.44721); end = (200, 0) - 10 x that = (191.056, -4.472)
//   target to the left, (100, 300): the arrow leaves the label's left side, (-28.4, 0)
static void test_callout(void) {
    an_scene *s = scene();
    an_font *mono = an_font_mono(s);
    an_obj *c0 = an_callout(s, &(an_callout_opts){ .text = "ab", .font = mono, .pos = { 300, 300 }, .target = { 500, 300 } });
    an_obj *c1 = an_callout(s, &(an_callout_opts){ .text = "ab", .font = mono, .pos = { 300, 300 }, .target = { 500, 300 }, .bend = 0.5f });
    an_obj *c2 = an_callout(s, &(an_callout_opts){ .text = "ab", .font = mono, .pos = { 300, 300 }, .target = { 100, 300 } });
    // the tip is the head polygon's first point; the shaft ends under the head,
    // 10 px back from the tip along the end tangent
    CHECK(c0->u.callout.head && an_obj_kind(c0->u.callout.head) == AN_KIND_POLYGON);
    an_vec2 p = path_pt(c0->u.callout.arrow, 0, 0), q = path_pt(c0->u.callout.arrow, 0, 1), tip = path_pt(c0->u.callout.head, 0, 0);
    CHECK_NEAR(p.x, 28.4, 1e-3);
    CHECK_NEAR(p.y, 0, 1e-4);
    CHECK_NEAR(tip.x, 190, 1e-3);
    CHECK_NEAR(tip.y, 0, 1e-4);
    CHECK_NEAR(q.x, 180, 1e-3);
    tip = path_pt(c1->u.callout.head, 0, 0);
    q = path_pt(c1->u.callout.arrow, 0, 1);
    CHECK_NEAR(tip.x, 191.056, 1e-3);
    CHECK_NEAR(tip.y, -4.472, 1e-3);
    CHECK_NEAR(q.x, 191.056 - 8.9443, 1e-3);
    CHECK_NEAR(q.y, -4.472 - 4.4721, 1e-3);
    p = path_pt(c2->u.callout.arrow, 0, 0);
    CHECK_NEAR(p.x, -28.4, 1e-3);
    // drawn on: create undrawn; an_draw on the callout reaches the arrow and the label
    an_obj *c3 = an_callout(s, &(an_callout_opts){ .text = "ab", .font = mono, .pos = { 300, 500 }, .target = { 500, 500 }, .undrawn = true });
    an_play(s, an_draw(c3, &(an_anim_opts){ .duration = 1, .ease = AN_EASE_LINEAR })); // reaches the arrow and the label
    an_seek(s, 0);
    CHECK(an_obj_get(c3->u.callout.arrow).draw == 0.0f && an_obj_get(c3->u.callout.label).draw == 0.0f);
    an_seek(s, 60);
    CHECK(an_obj_get(c3->u.callout.arrow).draw == 1.0f && an_obj_get(c3->u.callout.label).draw == 1.0f);
    an_scene_destroy(s);
}

// A pointer on array5 (cells 96): the tip's target is 0.5 x 96 + 0.75 x 96 = 120
// below the row, the label 0.95 x 96 = 91.2 below that (array space y 211.2). Label
// size 0.36 x 96 = 34.56: half its line box (1020 + 300) x 0.03456 / 2 = 22.81, + 8
// = 30.81 from the label's center. So locally the arrow runs from (0, -30.81) to
// (0, -91.2 + 4) = (0, -87.2) (gap 4: the head's tip; the shaft ends 10 px back,
// at -77.2). an_array_point to slot 4: the pointer ends
// exactly at x 192, y unchanged.
static void test_pointer(void) {
    an_scene *s = scene();
    an_obj *a = array5(s, (an_vec2){ 960, 600 }, NULL);
    an_obj *ptr = an_array_pointer(a, 2, &(an_pointer_opts){ .label = "i" });
    CHECK(ptr && an_obj_parent(ptr) == a && ptr->pos.x == 0.0f);
    CHECK_NEAR(ptr->pos.y, 211.2, 1e-3);
    const an_vec2 p = path_pt(ptr->u.callout.arrow, 0, 0), q = path_pt(ptr->u.callout.arrow, 0, 1);
    CHECK_NEAR(p.y, -30.81, 0.01);
    CHECK_NEAR(path_pt(ptr->u.callout.head, 0, 0).y, -87.2, 1e-3);
    CHECK_NEAR(q.y, -77.2, 1e-3);
    CHECK(p.x == 0.0f && q.x == 0.0f);
    an_play(s, an_array_point(ptr, 4, NULL));
    const an_vec2 e = pos_at(ptr, s, an_scene_length(s));
    CHECK(e.x == 192.0f && e.y == ptr->pos.y);
    an_scene_destroy(s);
}

// ---- script-time queries and named states ------------------------------------------------

// an_obj_at_cursor: a fade 1 -> 0.5 over 1 s (in-out) played async at 0, cursor
// moved to 0.5 s: the eased progress is 0.5, so opacity 0.75; an_seek elsewhere
// doesn't change it (it's the cursor, not the seek frame).
// States on array5: DONE on slot 2 (async, 0.3 s): the query says DONE at once
// (the state is a step at the start), while the fill at the cursor is still the
// base color; after a swap 2 <-> 4 the DONE element is in slot 4. Default dimmed:
// paper mixed 12% toward ink = (243 - 0.12 x 221, 237 - 0.12 x 215, 226 - 0.12 x 200)
// = (216.48, 211.2, 202) / 255.
static void test_cursor_and_states(void) {
    an_scene *s = scene();
    an_obj *r = an_rect(s, NULL);
    an_play_async(s, an_fade_to(r, 0.5f, &(an_anim_opts){ .duration = 1 }));
    an_wait(s, 0.5f);
    an_seek(s, 0);
    CHECK_NEAR(an_obj_at_cursor(r).opacity, 0.75, 1e-6);
    an_obj *a = array5(s, (an_vec2){ 960, 900 }, NULL);
    an_play_async(s, an_array_set_state(a, 2, AN_CELL_DONE, NULL));
    CHECK(an_array_state_at_cursor(a, 2) == AN_CELL_DONE && an_array_state_at_cursor(a, 1) == AN_CELL_BASE);
    const an_color cyan = AN_HEX(0x00C2FF), lime = AN_HEX(0xC6FF00);
    CHECK(an_obj_at_cursor(an_array_cell(a, 2)).fill.g == cyan.g); // the fill track has just started
    an_play(s, an_array_swap(a, 2, 4, NULL));
    CHECK(an_array_state_at_cursor(a, 4) == AN_CELL_DONE && an_array_state_at_cursor(a, 2) == AN_CELL_BASE);
    const an_obj_state st = an_obj_at_cursor(an_array_cell(a, 4));
    CHECK(st.fill.r == lime.r && st.fill.g == lime.g && st.fill.b == lime.b);
    an_play(s, an_array_set_state(a, 0, AN_CELL_DIMMED, NULL));
    const an_color d = an_obj_at_cursor(an_array_cell(a, 0)).fill;
    CHECK_NEAR(d.r * 255.0, 216.48, 1e-3);
    CHECK_NEAR(d.g * 255.0, 211.2, 1e-3);
    CHECK_NEAR(d.b * 255.0, 202.0, 1e-3);
    an_scene_destroy(s);
}

// ---- containers ---------------------------------------------------------------------------

// A group with children of stroke width 4 (c1) and 2 (c2), an overridden child
// (c3: its own setter at creation), a child with its own track from 0.7 s (c4),
// and a nested group holding c5 (width 3). The group animates its stroke width
// to 10, linear, 0.5 s .. 1.5 s (frames 30..90).
//   before frame 30: everyone their own value
//   frame 60 (f = 0.5): c1 = 4 + 0.5 x 6 = 7, c2 = 2 + 0.5 x 8 = 6, c5 = 3 + 0.5 x 7 = 6.5,
//     c3 = 4 (overridden), c4 = its own track from 0.7 s (frame 42) on: 1
//   frame 90 and later: c1, c2, c5 exactly 10
// Explicit start: an_draw(group) (from 0) at 2 s, linear 1 s: drawn children are at
// 0.5 at frame 150, 1 at frame 180.
static void test_containers(void) {
    an_scene *s = scene();
    an_obj *g = an_group(s, NULL), *g2 = an_group(s, &(an_group_opts){ .parent = g });
    an_obj *c1 = an_rect(s, &(an_rect_opts){ .parent = g, .stroke_width = 4 });
    an_obj *c2 = an_rect(s, &(an_rect_opts){ .parent = g, .stroke_width = 2 });
    an_obj *c3 = an_rect(s, &(an_rect_opts){ .parent = g, .stroke_width = 4 });
    an_obj *c4 = an_rect(s, &(an_rect_opts){ .parent = g, .stroke_width = 4 });
    an_obj *c5 = an_rect(s, &(an_rect_opts){ .parent = g2, .stroke_width = 3 });
    an_obj_set_stroke_width(c3, 4.0f); // overridden for good
    an_wait(s, 0.5f);
    an_play_async(s, an_stroke_width_to(g, 10, &(an_anim_opts){ .duration = 1, .ease = AN_EASE_LINEAR }));
    an_wait(s, 0.2f);
    an_play_async(s, an_stroke_width_to(c4, 1, &(an_anim_opts){ .duration = 0.0001f, .ease = AN_EASE_LINEAR }));
    an_wait(s, 1.3f); // cursor 2.0
    an_play(s, an_draw(g, &(an_anim_opts){ .duration = 1, .ease = AN_EASE_LINEAR }));
    an_seek(s, 29);
    CHECK(an_obj_get(c1).stroke_width == 4.0f && an_obj_get(c2).stroke_width == 2.0f && an_obj_get(c5).stroke_width == 3.0f);
    an_seek(s, 60);
    CHECK_NEAR(an_obj_get(c1).stroke_width, 7.0, 1e-5);
    CHECK_NEAR(an_obj_get(c2).stroke_width, 6.0, 1e-5);
    CHECK_NEAR(an_obj_get(c5).stroke_width, 6.5, 1e-5);
    CHECK(an_obj_get(c3).stroke_width == 4.0f && an_obj_get(c4).stroke_width == 1.0f);
    for (int64_t f = 90; f <= 120; f += 10) {
        an_seek(s, f);
        CHECK(an_obj_get(c1).stroke_width == 10.0f && an_obj_get(c2).stroke_width == 10.0f && an_obj_get(c5).stroke_width == 10.0f);
    }
    an_seek(s, 150);
    CHECK_NEAR(an_obj_get(c1).draw, 0.5, 1e-6);
    CHECK_NEAR(an_obj_get(c5).draw, 0.5, 1e-6);
    an_seek(s, 180);
    CHECK(an_obj_get(c1).draw == 1.0f);
    an_scene_destroy(s);

    // pointer options: hidden, offset 50 (the tip 50 px under the row's bottom edge,
    // 48 + 50 = 98 in the array's space), color on label and arrow
    s = scene();
    an_obj *a = array5(s, (an_vec2){ 960, 600 }, NULL);
    const an_color red = AN_HEX(0xFF0000);
    an_obj *ptr = an_array_pointer(a, 1, &(an_pointer_opts){ .label = "j", .hidden = true, .offset = 50, .color = red });
    an_seek(s, 0);
    CHECK(an_obj_get(ptr).opacity == 0.0f);
    const an_vec2 tip = path_pt(ptr->u.callout.head, 0, 0);
    CHECK_NEAR(ptr->pos.y + tip.y, 98.0 + 4.0, 1e-3); // the head's tip stops gap 4 short of its target
    CHECK(an_obj_get(ptr->u.callout.arrow).stroke.r == 1.0f && an_obj_get(ptr->u.callout.label).fill.r == 1.0f);
    an_scene_destroy(s);
}

// Code anchors: lines 10..12, "a = 1;   //@set", "b = 2;", "c = a; //@use-it":
// set = 10, use-it = 12; the shown lines are "a = 1;", "b = 2;", "c = a;" (marker
// and the spaces before it gone); an unknown name is 0; "//@" inside a line that
// isn't at its end is kept. A goto by anchor is the goto by number.
static void test_code_anchors(void) {
    an_scene *s = scene();
    an_obj *c = an_code(s, &(an_code_opts){ .first_line = 10,
                                            .code = "a = 1;   //@set\nb = 2; // x //@not end\nc = a; //@use-it" });
    CHECK(an_code_anchor(c, "set") == 10 && an_code_anchor(c, "use-it") == 12 && an_code_anchor(c, "nope") == 0);
    CHECK(strcmp(an_code_text(c, 10)->u.text.str, "a = 1;") == 0);
    CHECK(strcmp(an_code_text(c, 11)->u.text.str, "b = 2; // x //@not end") == 0);
    CHECK(strcmp(an_code_text(c, 12)->u.text.str, "c = a;") == 0);
    an_play(s, an_code_goto_anchor(c, "use-it", NULL));
    CHECK(an_code_current_line(c) == 12);
    an_scene_destroy(s);
}

// Bounds at the cursor (hand-computed): a 100 x 60 rect at (300, 200) with a 4 px
// outline: x 250..350, y 170..230, plus half the ink (2): (248, 168)..(352, 232);
// after a move to (400, 200) is played: 100 px to the right. array5 at (960, 900):
// slot 1 (x -96) is (816, 852)..(912, 948). Swap clearance: the arc heights from
// test_swap (249.02 for 1 <-> 3, 144 for 0 <-> 1); half that for an array scaled 0.5.
static void test_bounds(void) {
    an_scene *s = scene();
    an_obj *r = an_rect(s, &(an_rect_opts){ .pos = { 300, 200 }, .size = { 100, 60 }, .stroke_width = 4 });
    an_bounds b = an_obj_bounds_at_cursor(r);
    CHECK(b.min.x == 248.0f && b.min.y == 168.0f && b.max.x == 352.0f && b.max.y == 232.0f);
    an_play(s, an_move_to(r, (an_vec2){ 400, 200 }, NULL));
    b = an_obj_bounds_at_cursor(r);
    CHECK(b.min.x == 348.0f && b.max.x == 452.0f && b.min.y == 168.0f);
    an_obj *a = array5(s, (an_vec2){ 960, 900 }, NULL);
    b = an_array_slot_bounds(a, 1);
    CHECK(b.min.x == 816.0f && b.min.y == 852.0f && b.max.x == 912.0f && b.max.y == 948.0f);
    CHECK_NEAR(an_array_swap_clearance(a, 1, 3), 249.02, 0.01);
    CHECK_NEAR(an_array_swap_clearance(a, 0, 1), 144.0, 1e-4);
    an_obj *half = array5(s, (an_vec2){ 960, 500 }, NULL);
    an_obj_set_scale(half, (an_vec2){ 0.5f, 0.5f });
    CHECK_NEAR(an_array_swap_clearance(half, 1, 3), 124.51, 0.01);
    an_scene_destroy(s);
}

// ---- polygons -------------------------------------------------------------------------------

// Chaos 0, black fills.
//   L-shape (100,100) (200,100) (200,140) (140,140) (140,200) (100,200): (120,120)
//   and (150,120) inside, the notch (170,170) paper, (99,150) paper.
//   Pentagram, one path, points 80 px around (400, 200) at -90 + k x 144 degrees:
//   the center has winding 2 (filled under non-zero; even-odd would leave it
//   empty); a tip region (400, 135) is inside (winding 1); (400, 290) is outside.
//   An undrawn outlined square (500..560): its fill is absent at draw 0.5 (frame
//   30 of a 1 s linear draw) and present at frame 60.
static void test_polygon(void) {
    an_scene *s = scene();
    an_set_chaos(s, 0.0f);
    an_polygon(s, &(an_polygon_opts){ .points = (an_vec2[]){ { 100, 100 }, { 200, 100 }, { 200, 140 }, { 140, 140 },
                                                             { 140, 200 }, { 100, 200 } }, .count = 6 });
    an_vec2 star[5];
    for (int k = 0; k < 5; k++) {
        const double th = -1.5707963267948966 + k * 4.0 * 3.141592653589793 / 5.0;
        star[k] = (an_vec2){ (float)(80.0 * cos(th)), (float)(80.0 * sin(th)) };
    }
    an_polygon(s, &(an_polygon_opts){ .pos = { 400, 200 }, .points = star, .count = 5 });
    an_obj *sq = an_polygon(s, &(an_polygon_opts){ .undrawn = true, .stroke = AN_HEX(0x16161A), .stroke_width = 4,
                                                   .points = (an_vec2[]){ { 500, 100 }, { 560, 100 }, { 560, 160 }, { 500, 160 } },
                                                   .count = 4, .fill = AN_HEX(0x0078BF) });
    an_play(s, an_draw(sq, &(an_anim_opts){ .duration = 1, .ease = AN_EASE_LINEAR }));
    render(s, 30);
    CHECK(is(120, 120, 0, 0, 0) && is(150, 120, 0, 0, 0) && PAPER(170, 170) && PAPER(99, 150));
    CHECK(is(400, 200, 0, 0, 0) && is(400, 135, 0, 0, 0) && PAPER(400, 290));
    CHECK(PAPER(530, 130));
    render(s, 60);
    CHECK(is(530, 130, 0, 120, 191));
    an_scene_destroy(s);
}

int main(void) {
    if (!ani_gfx_setup_headless()) {
        fprintf(stderr, "test_explainer: headless Metal setup failed\n");
        return 1;
    }
    CHECK(ani_offscreen_make_ds(&g_off, W, H, 4));
    CHECK(ani_renderer_init(&g_r, SG_PIXELFORMAT_RGBA8, SG_PIXELFORMAT_DEPTH_STENCIL, 4));
    g_px = malloc(ani_offscreen_frame_bytes(&g_off));

    test_arc_track();
    test_layout();
    test_swap();
    test_seek_middle();
    test_highlight();
    test_fade_one_layer();
    test_paper_under_default();
    test_code_marker();
    test_callout();
    test_pointer();
    test_cursor_and_states();
    test_containers();
    test_code_anchors();
    test_bounds();
    test_polygon();

    free(g_px);
    ani_renderer_shutdown(&g_r);
    ani_offscreen_destroy(&g_off);
    ani_gfx_shutdown();
    return check_done("test_explainer");
}
