// Timeline semantics (docs/timeline.md): cursor arithmetic, tick->frame
// conversion, seek order independence, take-over handoff, setters, lifetime,
// zero-opacity skipping. CPU only.
#include "anown.h"
#include "check.h"
#include "ease_internal.h"
#include "tess_internal.h"
#include "timeline_internal.h"

#include <stdlib.h>
#include <string.h>

#define S_TICKS(sec) ((int64_t)((sec) * 1000000.0 + 0.5))

static an_anim move_x(an_obj *o, float x, float dur) {
    return an_move_to(o, (an_vec2){ x, 0 }, &(an_anim_opts){ .duration = dur, .ease = AN_EASE_LINEAR });
}

static void test_cursor(void) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .fps = 60 });
    an_obj *a = an_rect(s, NULL);
    an_obj *b = an_rect(s, NULL);
    CHECK(s->cursor == 0 && an_scene_length(s) == 0);

    an_play(s, move_x(a, 100, 1.0f)); // [0, 1]
    CHECK(s->cursor == S_TICKS(1.0));
    an_wait(s, 0.5f);                 // 1.5
    CHECK(s->cursor == S_TICKS(1.5));
    an_play_async(s, move_x(b, 50, 2.0f)); // [1.5, 3.5], cursor stays
    CHECK(s->cursor == S_TICKS(1.5));
    CHECK(s->end == S_TICKS(3.5));
    an_par(s, 2, (an_anim[]){ move_x(a, 0, 0.25f), an_fade_to(a, 0.5f, &(an_anim_opts){ .duration = 0.75f }) });
    CHECK(s->cursor == S_TICKS(2.25)); // longest of par
    an_stagger(s, 3, (an_anim[]){ an_rotate_by(a, 1, &(an_anim_opts){ .duration = 0.5f }),
                                  an_rotate_by(b, 1, &(an_anim_opts){ .duration = 0.5f }),
                                  an_scale_to(a, (an_vec2){ 2, 2 }, &(an_anim_opts){ .duration = 0.1f }) },
               0.2f);
    // starts 2.25, 2.45, 2.65; ends 2.75, 2.95, 2.75 -> cursor at the latest end
    CHECK(s->cursor == S_TICKS(2.95));
    CHECK_NEAR(an_scene_time(s), 2.95, 1e-9);
    // length: the async move ends at 3.5 s, after the cursor -> frame 210
    CHECK(an_scene_length(s) == 210);
    an_scene_destroy(s);
}

// Integer ticks: no drift from per-call rounding.
static void test_ticks_no_drift(void) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .fps = 24 });
    for (int i = 0; i < 10; i++) an_wait(s, 0.1f);
    CHECK(s->cursor == 1000000);
    CHECK(an_scene_length(s) == 24); // exactly 24 frames, last index 24 inclusive
    an_scene_destroy(s);

    // a track ending between frames shows its end value from the next frame on
    an_scene *t = an_scene_create(&(an_scene_desc){ .fps = 24 });
    an_obj *o = an_rect(t, NULL);
    an_play(t, move_x(o, 240, 0.1f)); // ends at 2.4 frames
    CHECK(an_scene_length(t) == 3);
    an_seek(t, 2);
    CHECK(an_obj_get(o).pos.x < 240.0f);
    an_seek(t, 3);
    CHECK(an_obj_get(o).pos.x == 240.0f);
    an_scene_destroy(t);
}

// Build a busy scene: springs, easing, colors, z changes, groups, late births.
static an_scene *busy_scene(an_obj **probe) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .fps = 30, .seed = 3 });
    an_obj *g = an_group(s, &(an_group_opts){ .pos = { 960, 540 } });
    an_obj *items[12];
    for (int i = 0; i < 12; i++) {
        items[i] = an_circle(s, &(an_circle_opts){ .parent = g, .pos = { (float)(i * 60 - 330), 0 }, .radius = 20,
                                                   .fill = AN_HEX(0x0078BF) });
    }
    an_anim drops[12];
    for (int i = 0; i < 12; i++) {
        const float dy = an_rng_range(an_obj_rng(items[i]), -200, 200);
        drops[i] = an_move_by(items[i], (an_vec2){ 0, dy }, &(an_anim_opts){ .ease = an_spring(0.4f, 0.7f) });
    }
    an_stagger(s, 12, drops, 0.05f);
    an_play_async(s, an_rotate_by(g, 3.0f, &(an_anim_opts){ .duration = 2 }));
    an_obj *late = an_rect(s, &(an_rect_opts){ .pos = { 100, 100 }, .hidden = true, .z = 5 });
    an_play(s, an_fade_to(late, 1, NULL));
    an_par(s, 2, (an_anim[]){ an_fill_to(items[3], AN_HEX(0xFF48B0), NULL), an_scale_to(g, (an_vec2){ 0.5f, 1.5f }, NULL) });
    an_obj_set_z(items[0], 10);
    an_wait(s, 0.3f);
    an_play(s, an_move_by(items[5], (an_vec2){ 300, 0 }, &(an_anim_opts){ .ease = an_spring(1.0f, 0.4f) }));
    *probe = items[5];
    return s;
}

static bool state_eq(an_obj_state a, an_obj_state b) {
    return memcmp(&a, &b, sizeof a) == 0;
}

// seek(N) depends only on N: in order, reverse and shuffled give identical
// states and identical meshes.
static void test_seek_order_independent(void) {
    an_obj *probe;
    an_scene *s = busy_scene(&probe);
    const int64_t L = an_scene_length(s);
    CHECK(L > 60);
    const int64_t n = L + 1;
    an_obj_state *fwd = calloc((size_t)n, sizeof *fwd);
    uint32_t *mesh_hash = calloc((size_t)n, sizeof *mesh_hash);
    ani_mesh m;
    ani_mesh_init(&m);

    for (int64_t f = 0; f < n; f++) {
        an_seek(s, f);
        fwd[f] = an_obj_get(probe);
        CHECK(ani_tess_scene(&m, s, f));
        uint32_t h = 2166136261u; // FNV-1a over the vertex bytes
        const unsigned char *p = (const unsigned char *)m.v;
        for (size_t k = 0; k < (size_t)m.count * sizeof(ani_vertex); k++) h = (h ^ p[k]) * 16777619u;
        mesh_hash[f] = h;
    }

    int mismatches = 0;
    for (int64_t f = n - 1; f >= 0; f--) { // reverse
        an_seek(s, f);
        if (!state_eq(an_obj_get(probe), fwd[f])) mismatches++;
    }
    an_rng rng = an_rng_make(1234, 0); // shuffled order
    int64_t *order = malloc((size_t)n * sizeof *order);
    for (int64_t i = 0; i < n; i++) order[i] = i;
    for (int64_t i = n - 1; i > 0; i--) {
        const int64_t j = an_rng_below(&rng, (uint32_t)(i + 1));
        const int64_t t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    for (int64_t i = 0; i < n; i++) {
        const int64_t f = order[i];
        an_seek(s, f);
        if (!state_eq(an_obj_get(probe), fwd[f])) mismatches++;
        CHECK(ani_tess_scene(&m, s, f));
        uint32_t h = 2166136261u;
        const unsigned char *p = (const unsigned char *)m.v;
        for (size_t k = 0; k < (size_t)m.count * sizeof(ani_vertex); k++) h = (h ^ p[k]) * 16777619u;
        if (h != mesh_hash[f]) mismatches++;
    }
    CHECK(mismatches == 0);

    // and a second scene built from the same script matches frame for frame
    an_obj *probe2;
    an_scene *s2 = busy_scene(&probe2);
    for (int64_t f = 0; f < n; f += 7) {
        an_seek(s2, f);
        CHECK(state_eq(an_obj_get(probe2), fwd[f]));
    }
    an_scene_destroy(s2);

    free(order);
    free(fwd);
    free(mesh_hash);
    ani_mesh_free(&m);
    an_scene_destroy(s);
}

// Take-over: a later track replaces a running one without a jump; spring to
// spring also hands over velocity.
static void test_takeover(void) {
    // Very fine frames (10 us) so one-sided differences approximate velocity
    // closely even under the springs' strong accelerations (~5e4 px/s^2).
    const int fps = 100000;
    an_scene *s = an_scene_create(&(an_scene_desc){ .fps = fps });
    an_obj *o = an_rect(s, NULL);
    const an_ease first = an_spring(0.3f, 1.0f);
    an_play_async(s, an_move_to(o, (an_vec2){ 1000, 0 }, &(an_anim_opts){ .ease = first }));
    an_wait(s, 0.4f); // interrupt mid-flight, at frame 40000
    an_play(s, an_move_to(o, (an_vec2){ -500, 0 }, &(an_anim_opts){ .ease = an_spring(0.6f, 0.8f) }));

    // the new track stored the first spring's analytic velocity at 0.4 s
    double d, v_exact;
    ani_spring_eval(ani_spring_params(first), -1000.0, 0.0, 0.4, &d, &v_exact);
    const ani_channel *ch = o->ch[AN_PROP_POS];
    CHECK(ch->count == 2);
    CHECK_NEAR(ch->tracks[1].v0.v[0], v_exact, 1e-9);
    CHECK(v_exact > 100.0); // was moving right fast

    const int64_t k = 40000;
    double x[3];
    for (int i = 0; i < 3; i++) {
        an_seek(s, k - 1 + i);
        x[i] = an_obj_get(o).pos.x;
    }
    const double v_before = (x[1] - x[0]) * fps; // old spring, just before
    const double v_after = (x[2] - x[1]) * fps;  // new spring, just after
    CHECK_NEAR(x[1], 1000.0 + d, 1e-3);           // no jump: new track starts where old was
    CHECK_NEAR(v_before, v_exact, fabs(v_exact) * 0.01);
    CHECK_NEAR(v_after, v_exact, fabs(v_exact) * 0.01); // velocity carried over
    an_seek(s, an_scene_length(s));
    CHECK(an_obj_get(o).pos.x == -500.0f);
    an_scene_destroy(s);

    // eased take-over: position continuous, starts from rest (no velocity used)
    an_scene *e = an_scene_create(&(an_scene_desc){ .fps = 60 });
    an_obj *p = an_rect(e, NULL);
    an_play_async(e, move_x(p, 600, 1.0f));
    an_wait(e, 0.5f); // at 300
    an_play(e, an_move_to(p, (an_vec2){ 0, 0 }, &(an_anim_opts){ .duration = 1 }));
    an_seek(e, 30);
    CHECK_NEAR(an_obj_get(p).pos.x, 300.0, 1e-3);
    CHECK(p->ch[AN_PROP_POS]->tracks[1].v0.v[0] == 0.0);
    an_seek(e, 31);
    CHECK_NEAR(an_obj_get(p).pos.x, 300.0, 0.1); // in-out starts slowly
    an_scene_destroy(e);
}

// Setters: steps at the cursor; during a running track they cut it off.
static void test_setters(void) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .fps = 30 });
    an_obj *o = an_rect(s, &(an_rect_opts){ .fill = AN_HEX(0x000000) });
    an_obj_set_pos(o, (an_vec2){ 5, 5 }); // cursor 0, no tracks: base write
    CHECK(o->ch[AN_PROP_POS] == NULL && o->pos.x == 5.0f);

    an_wait(s, 1.0f);
    an_obj_set_fill(o, AN_HEX(0xFF48B0)); // step at exactly frame 30
    an_seek(s, 29);
    CHECK(an_obj_get(o).fill.r == 0.0f);
    an_seek(s, 30);
    CHECK(an_obj_get(o).fill.r == 1.0f);
    CHECK(o->fill.r == 0.0f); // base untouched

    // a setter during a running track takes over from the cursor on
    an_play_async(s, move_x(o, 1000, 2.0f)); // 1 s .. 3 s
    an_wait(s, 1.0f);                         // 2 s: x would be 502.5 by now
    an_obj_set_pos(o, (an_vec2){ -50, 0 });
    an_seek(s, 59);
    CHECK(an_obj_get(o).pos.x > 400.0f);
    for (int64_t f = 60; f <= 120; f += 15) {
        an_seek(s, f);
        CHECK(an_obj_get(o).pos.x == -50.0f);
    }
    an_scene_destroy(s);
}

// Lifetime and opacity: objects don't exist before creation; zero effective
// opacity emits nothing (including children of a faded group).
static void test_lifetime_and_opacity(void) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .fps = 30 });
    an_set_chaos(s, 0); // structural counts: rects are plain quads (no boil)
    an_rect(s, NULL);
    an_wait(s, 1.0f);
    an_obj *late = an_rect(s, NULL);
    ani_mesh m;
    ani_mesh_init(&m);
    an_seek(s, 29);
    CHECK(!an_obj_get(late).exists);
    CHECK(ani_tess_scene(&m, s, 29) && m.count == 6 + 6);
    CHECK(ani_tess_scene(&m, s, 30) && m.count == 6 + 12);

    an_obj *g = an_group(s, NULL);
    for (int i = 0; i < 5; i++) an_circle(s, &(an_circle_opts){ .parent = g });
    an_obj *hidden = an_rect(s, &(an_rect_opts){ .hidden = true });
    CHECK(ani_tess_scene(&m, s, 30));
    const int with_group = m.count;
    an_obj_set_opacity(g, 0.0f); // cursor 1 s == the group's birth, no tracks: base write
    CHECK(ani_tess_scene(&m, s, 30));
    CHECK(m.count == 6 + 12); // group subtree and the hidden rect emit nothing
    CHECK(with_group > m.count);

    // opacity < 1 isolates: the group gets a layer, the child a nested layer,
    // each composited with its own opacity (0.5 * 0.5 overall). Paint colors keep
    // their own alpha; opacity is never baked into vertices.
    an_obj *h = an_group(s, &(an_group_opts){ .pos = { 0, 0 } });
    an_obj_set_opacity(h, 0.5f);
    an_rect(s, &(an_rect_opts){ .parent = h, .fill = AN_HEX(0xFFFFFF) });
    an_obj *c = h->first_child;
    an_obj_set_opacity(c, 0.5f);
    CHECK(ani_tess_scene(&m, s, 30));
    CHECK(m.nlayers == 2);
    CHECK(m.layers[0].obj == h && m.layers[0].parent == ANI_TARGET_MAIN && m.layers[0].opacity == 0.5f);
    CHECK(m.layers[1].obj == c && m.layers[1].parent == 0 && m.layers[1].opacity == 0.5f);
    CHECK(m.v[m.count - 1].a == 1.0f);
    (void)hidden;
    ani_mesh_free(&m);
    an_scene_destroy(s);
}

// Chaos: a scene-target property and an object multiplier, through the same
// (target, property) tracks as everything else. Nothing renders with it yet (5A).
static void test_chaos(void) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .fps = 60 });
    an_obj *g = an_group(s, NULL);
    an_obj *c = an_rect(s, &(an_rect_opts){ .parent = g });
    an_obj *other = an_rect(s, NULL);
    an_seek(s, 0);
    CHECK(an_scene_chaos(s) == 1.0f && an_obj_chaos(c) == 1.0f); // defaults: the look is on

    an_set_chaos(s, 0.8f); // _Generic: scene
    an_set_chaos(g, 0.5f); // _Generic: object
    an_set_chaos(c, 0.4f);
    CHECK(s->ch[AN_PROP_CHAOS] == NULL && s->chaos == 0.8f); // cursor 0, no tracks: base writes
    CHECK_NEAR(an_obj_chaos(c), 0.8 * 0.5 * 0.4, 1e-6);    // scene x group x object
    CHECK_NEAR(an_obj_chaos(other), 0.8, 1e-6);

    an_play(s, an_scene_chaos_to(s, 0.0f, &(an_anim_opts){ .duration = 1, .ease = AN_EASE_LINEAR }));
    an_obj_set_chaos(g, 1.0f); // at 1 s: an instant change
    an_play(s, an_chaos_to(c, 1.0f, &(an_anim_opts){ .duration = 1, .ease = AN_EASE_LINEAR }));
    an_seek(s, 30);
    CHECK_NEAR(an_scene_chaos(s), 0.4, 1e-6);
    CHECK_NEAR(an_obj_chaos(c), 0.4 * 0.5 * 0.4, 1e-6);
    an_seek(s, 60);
    CHECK(an_scene_chaos(s) == 0.0f);
    CHECK(an_obj_chaos(c) == 0.0f);
    an_seek(s, 90); // scene 0: everything is calm regardless of multipliers
    CHECK(an_obj_chaos(c) == 0.0f);

    // values clamp to [0, 1]; scene targets accept only scene properties
    an_set_chaos(other, 3.0f);
    CHECK(ani_sample(ani_obj_prop(other, AN_PROP_CHAOS), ani_frame_time(s, 200)).v[0] == 1.0); // clamped
    an_anim bad = an_scene_chaos_to(s, 1.0f, NULL);
    bad.prop = AN_PROP_POS;
    const int64_t before = s->cursor;
    an_play(s, bad); // not a scene property: ignored
    CHECK(s->cursor == before && s->ch[AN_PROP_POS] == NULL);

    // seek order independence
    float fwd[121];
    for (int f = 0; f <= 120; f++) {
        an_seek(s, f);
        fwd[f] = an_obj_chaos(c);
    }
    int mism = 0;
    for (int f = 120; f >= 0; f -= 7) {
        an_seek(s, f);
        if (an_obj_chaos(c) != fwd[f]) mism++;
    }
    CHECK(mism == 0);
    an_scene_destroy(s);
}

// Handles: what playing returns (seconds; ticks exact).
//   cursor 0.5: an_play(fade, 1 s) -> start 0.5, end 1.5, cursor 1.5
//   an_play_async(move, 2 s) -> 1.5 .. 3.5, cursor stays 1.5
//   an_stagger(3 anims of 1 s, gap 0.25) -> 1.5 .. 1.5 + 0.5 + 1 = 3.0, cursor 3.0
//   an_wait_until(async) -> cursor 3.5; an_wait_until(the first) -> stays 3.5
//   a swap: 0.9 s from the cursor
static void test_handles(void) {
    an_scene *s = an_scene_create(NULL);
    an_obj *o = an_rect(s, NULL), *p = an_rect(s, NULL);
    an_wait(s, 0.5f);
    const an_handle h1 = an_play(s, an_fade_to(o, 0.5f, &(an_anim_opts){ .duration = 1 }));
    CHECK(h1.scene == s && h1.start == 0.5 && h1.end == 1.5 && an_scene_time(s) == 1.5);
    const an_handle h2 = an_play_async(s, an_move_by(p, (an_vec2){ 10, 0 }, &(an_anim_opts){ .duration = 2 }));
    CHECK(h2.start == 1.5 && h2.end == 3.5 && an_scene_time(s) == 1.5);
    an_anim three[3] = { an_fade_to(o, 1, NULL), an_fade_to(p, 1, NULL), an_rotate_by(o, 1, NULL) };
    const an_handle h3 = an_stagger(s, 3, three, 0.25f);
    CHECK(h3.start == 1.5 && h3.end == 3.0 && an_scene_time(s) == 3.0);
    an_wait_until(h2);
    CHECK(an_scene_time(s) == 3.5);
    an_wait_until(h1);
    CHECK(an_scene_time(s) == 3.5);
    static const int v[3] = { 1, 2, 3 };
    an_obj *a = an_array(s, &(an_array_opts){ .pos = { 960, 800 }, .values = v, .count = 3 });
    const an_handle sw = an_play_async(s, an_array_swap(a, 0, 2, NULL));
    CHECK(sw.start == 3.5 && sw.end == 4.4);
    an_scene_destroy(s);
}

int main(void) {
    test_chaos();
    test_cursor();
    test_ticks_no_drift();
    test_seek_order_independent();
    test_takeover();
    test_setters();
    test_lifetime_and_opacity();
    test_handles();
    return check_done("test_timeline");
}
