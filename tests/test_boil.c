// Line boil (docs/layers.md, 5C): keyed by (object RNG stream, arc-length bucket,
// (N * 12) / fps) through an_hash. CPU only (tessellator and triangulator output).
#include "anown.h"
#include "check.h"
#include "stroke_internal.h"
#include "scene_internal.h"
#include "timeline_internal.h"

#include <stdlib.h>
#include <string.h>

static const an_seg PATH[] = { AN_MOVE(100, 300), AN_CUBIC(400, 50, 700, 550, 1000, 300), AN_LINE(1300, 420),
                               AN_CUBIC(1500, 500, 1700, 200, 1800, 280) };

// A scene with a mix of boiling things: strokes (tapered, overshoot, shadow), a
// stroked rect and circle with fills, an arrow, a group.
static an_scene *mixed(int fps, an_style style) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .fps = fps, .seed = 31, .style = style });
    an_stroke(s, &(an_stroke_opts){ .name = "wave", .segs = PATH, .seg_count = 4, .stroke = AN_HEX(0x16161A), .stroke_width = 8,
                                    .brush = { .taper_in = 30, .taper_out = 60, .overshoot = 0.6f }, .shadow = AN_ON });
    an_rect(s, &(an_rect_opts){ .pos = { 400, 700 }, .size = { 300, 200 }, .fill = AN_HEX(0xFF2E88),
                                .stroke = AN_HEX(0x16161A), .stroke_width = 8 });
    an_obj *g = an_group(s, &(an_group_opts){ .pos = { 1200, 750 }, .rotation = 0.3f });
    an_circle(s, &(an_circle_opts){ .parent = g, .radius = 120, .fill = AN_HEX(0x00C2FF), .stroke = AN_HEX(0x16161A), .stroke_width = 6 });
    an_arrow(s, &(an_arrow_opts){ .from = { 700, 900 }, .to = { 1000, 820 }, .stroke = AN_HEX(0x16161A), .stroke_width = 6 });
    return s;
}

static bool same_mesh(const ani_mesh *a, const ani_mesh *b) {
    return a->count == b->count && a->ncmds == b->ncmds &&
           memcmp(a->v, b->v, (size_t)a->count * sizeof(ani_vertex)) == 0 &&
           memcmp(a->cmds, b->cmds, (size_t)a->ncmds * sizeof(ani_cmd)) == 0;
}

// chaos = 0 is byte-identical to no boil: scene chaos 0, and object chaos 0
// inside a scene at chaos 1.
static void test_chaos_zero_identical(void) {
    ani_mesh a, b;
    ani_mesh_init(&a);
    ani_mesh_init(&b);
    an_scene *on = mixed(60, (an_style){ 0 });
    an_scene *off = mixed(60, (an_style){ .boil_off = true });
    an_set_chaos(on, 0);
    an_set_chaos(off, 0);
    for (int64_t f = 0; f < 30; f += 7) {
        CHECK(ani_tess_scene(&a, on, f) && ani_tess_scene(&b, off, f));
        CHECK(same_mesh(&a, &b));
    }
    an_scene_destroy(on);
    an_scene_destroy(off);

    // per object: every object at chaos 0 in a chaos-1 scene vs boil off
    an_scene *s1 = mixed(60, (an_style){ 0 });
    an_scene *s2 = mixed(60, (an_style){ .boil_off = true });
    for (an_obj *o = s1->root.first_child; o; o = o->next_sibling) an_set_chaos(o, 0);
    for (an_obj *o = s2->root.first_child; o; o = o->next_sibling) an_set_chaos(o, 0);
    CHECK(ani_tess_scene(&a, s1, 3) && ani_tess_scene(&b, s2, 3));
    CHECK(same_mesh(&a, &b));
    // and boil really is on at chaos 1 (otherwise the test above proves nothing)
    an_scene *s3 = mixed(60, (an_style){ 0 });
    CHECK(ani_tess_scene(&a, s3, 3) && ani_tess_scene(&b, s2, 3));
    CHECK(a.count != b.count);
    an_scene_destroy(s1);
    an_scene_destroy(s2);
    an_scene_destroy(s3);
    ani_mesh_free(&a);
    ani_mesh_free(&b);
}

// No crawl: a point on the already-drawn part of a stroke has the same
// displacement at progress 0.5 and 1.0 (same frame, so the same 12 Hz step).
static void test_no_crawl(void) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .seed = 31 });
    an_obj *o = an_stroke(s, &(an_stroke_opts){ .name = "wave", .segs = PATH, .seg_count = 4, .stroke_width = 8,
                                                .brush = { .taper_in = 30, .taper_out = 60 } });
    const ani_boil boil = { .amp = 3.0f, .spacing = 15.0f, .seed = 31, .stream = o->rng_key, .step = 7 };
    ani_rib_log half = { 0 }, full = { 0 };
    ani_mesh m;
    ani_mesh_init(&m);
    const ani_stroke_in base = { .path = o->path, .world = { 1, 0, 0, 1, 0, 0 }, .width = 8, .color = AN_BLACK,
                                 .brush = &o->brush, .boil = boil };
    ani_stroke_in in = base;
    in.progress = 0.5f;
    CHECK(ani_stroke_emit(&m, &in, &half));
    in.progress = 1.0f;
    m.count = 0;
    CHECK(ani_stroke_emit(&m, &in, &full));
    // compare every rib of the half-drawn stroke up to one boil bucket before its cut
    const float cut = 0.5f * o->path->total;
    int compared = 0, moved = 0;
    for (int i = 0; i < half.count; i++) {
        const ani_rib *a = &half.ribs[i];
        if (a->s > cut - 15.0f) continue;
        for (int j = 0; j < full.count; j++) {
            const ani_rib *b = &full.ribs[j];
            if (b->sub == a->sub && b->s == a->s) {
                compared++;
                moved += !(a->p.x == b->p.x && a->p.y == b->p.y && a->h == b->h);
                break;
            }
        }
    }
    printf("  no crawl: %d drawn points compared between progress 0.5 and 1.0, %d moved\n", compared, moved);
    CHECK(compared > 50 && moved == 0);
    // and across many intermediate progresses
    for (int k = 1; k <= 9; k++) {
        ani_rib_log part = { 0 };
        m.count = 0;
        in.progress = 0.1f * (float)k;
        CHECK(ani_stroke_emit(&m, &in, &part));
        const float c = in.progress * o->path->total;
        for (int i = 0; i < part.count; i++) {
            if (part.ribs[i].s > c - 15.0f) continue;
            for (int j = 0; j < full.count; j++) {
                if (full.ribs[j].s == part.ribs[i].s) {
                    if (!(full.ribs[j].p.x == part.ribs[i].p.x && full.ribs[j].p.y == part.ribs[i].p.y))
                        fprintf(stderr, "  progress %.1f: point at s=%.3f moved (%.4f,%.4f) vs (%.4f,%.4f); rib %d of %d, cut at %.1f\n",
                                in.progress, part.ribs[i].s, part.ribs[i].p.x, part.ribs[i].p.y, full.ribs[j].p.x, full.ribs[j].p.y, i, part.count, c);
                    CHECK(full.ribs[j].p.x == part.ribs[i].p.x && full.ribs[j].p.y == part.ribs[i].p.y);
                    break;
                }
            }
        }
        free(part.ribs);
    }
    free(half.ribs);
    free(full.ribs);
    ani_mesh_free(&m);
    an_scene_destroy(s);
}

// 12 Hz in integer tick math. A static scene, so only the boil changes.
// Hand-listed: step = floor(N * 12 / fps)
//   24 fps: steps 0 0 1 1 2 2 ...       change at 2, 4, 6
//   30 fps: steps 0 0 0 1 1 2 2 2 3 3 4 change at 3, 5, 8, 10
//   60 fps: steps 0 0 0 0 0 1 ...       change at 5, 10
// Same instant at different fps = same step = identical triangles:
//   24 fps frame 2, 30 fps frame 3, 60 fps frame 5 are all step 1
static void test_12hz(void) {
    ani_mesh a, b;
    ani_mesh_init(&a);
    ani_mesh_init(&b);
    const int fps[3] = { 24, 30, 60 };
    const char *changes[3] = { "0011223344", "00011222334", "00000111112" }; // step per frame 0..
    for (int k = 0; k < 3; k++) {
        an_scene *s = mixed(fps[k], (an_style){ 0 });
        const int n = (int)strlen(changes[k]);
        for (int f = 0; f + 1 < n; f++) {
            CHECK(ani_tess_scene(&a, s, f) && ani_tess_scene(&b, s, f + 1));
            const bool should_change = changes[k][f] != changes[k][f + 1];
            if (same_mesh(&a, &b) == should_change) {
                fprintf(stderr, "  %d fps: frames %d -> %d %s\n", fps[k], f, f + 1,
                        should_change ? "should change" : "should not change");
                CHECK(false);
            }
        }
        an_scene_destroy(s);
    }
    an_scene *s24 = mixed(24, (an_style){ 0 }), *s30 = mixed(30, (an_style){ 0 }), *s60 = mixed(60, (an_style){ 0 });
    CHECK(ani_tess_scene(&a, s24, 2) && ani_tess_scene(&b, s30, 3));
    CHECK(same_mesh(&a, &b));
    CHECK(ani_tess_scene(&b, s60, 5));
    CHECK(same_mesh(&a, &b));
    CHECK(ani_tess_scene(&b, s60, 4)); // step 0 at 60 fps
    CHECK(!same_mesh(&a, &b));
    an_scene_destroy(s24);
    an_scene_destroy(s30);
    an_scene_destroy(s60);
    ani_mesh_free(&a);
    ani_mesh_free(&b);
}

// Keyed by the object's stream: same name -> same noise in another scene;
// different name -> different noise. Closed paths are continuous at the seam.
// The amplitude bounds the displacement.
static void test_keying_and_shape(void) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .seed = 31 });
    an_obj *a = an_stroke(s, &(an_stroke_opts){ .name = "a", .segs = PATH, .seg_count = 4 });
    an_obj *b = an_stroke(s, &(an_stroke_opts){ .name = "b", .segs = PATH, .seg_count = 4 });
    an_scene *t = an_scene_create(&(an_scene_desc){ .seed = 31 });
    an_stroke(t, &(an_stroke_opts){ .name = "zzz", .segs = PATH, .seg_count = 4 }); // shifts creation ids
    an_obj *a2 = an_stroke(t, &(an_stroke_opts){ .name = "a", .segs = PATH, .seg_count = 4 });
    const ani_boil ba = { .amp = 2, .spacing = 15, .seed = 31, .stream = a->rng_key, .step = 3 };
    ani_boil bb = ba, ba2 = ba;
    bb.stream = b->rng_key;
    ba2.stream = a2->rng_key;
    int same = 0, diff = 0;
    double maxabs = 0;
    for (double x = 0; x < a->path->total; x += 3.7) {
        const double da = ani_boil_disp(&ba, 0, x, a->path->subs[0].len, false);
        same += da == ani_boil_disp(&ba2, 0, x, a->path->subs[0].len, false);
        diff += da != ani_boil_disp(&bb, 0, x, a->path->subs[0].len, false);
        maxabs = fmax(maxabs, fabs(da));
    }
    const int n = (int)(a->path->total / 3.7) + 1;
    CHECK(same == n);       // same name, other scene, other id: identical
    CHECK(diff > n * 9 / 10);
    CHECK(maxabs <= 2.0 && maxabs > 1.0);

    // closed: the noise wraps, so s = 0 and s = len agree
    an_obj *r = an_rect(s, &(an_rect_opts){ .size = { 157, 83 } }); // perimeter 480: 32 buckets of 15
    const double len = r->path->subs[0].len;
    const ani_boil br = { .amp = 2, .spacing = 15, .seed = 31, .stream = r->rng_key, .step = 0 };
    CHECK(fabs(ani_boil_disp(&br, 0, 0.0, len, true) - ani_boil_disp(&br, 0, len, len, true)) < 1e-9);
    CHECK(fabs(ani_boil_disp(&br, 0, 1e-4, len, true) - ani_boil_disp(&br, 0, len - 1e-4, len, true)) < 1e-3);

    // fill ring and outline: the same displaced points (rect, untapered = loop)
    ani_mesh m;
    ani_mesh_init(&m);
    ani_mesh ring_mesh;
    ani_mesh_init(&ring_mesh);
    const an_vec2 *ring;
    const int nr = ani_boil_ring(&ring_mesh, r->path, (ani_affine){ 1, 0, 0, 1, 0, 0 }, &br, &ring);
    ani_rib_log log = { 0 };
    const ani_stroke_in in = { .path = r->path, .world = { 1, 0, 0, 1, 0, 0 }, .width = 4, .progress = 1,
                               .color = AN_BLACK, .boil = br };
    CHECK(ani_stroke_emit(&m, &in, &log));
    CHECK(nr == log.count && nr > 30);
    int off = 0;
    for (int i = 0; i < nr && i < log.count; i++) off += fabsf(ring[i].x - log.ribs[i].p.x) > 1e-4f || fabsf(ring[i].y - log.ribs[i].p.y) > 1e-4f;
    CHECK(off == 0);
    free(log.ribs);
    ani_mesh_free(&ring_mesh);
    ani_mesh_free(&m);
    an_scene_destroy(s);
    an_scene_destroy(t);
}

// Displacement is a function of arc length, not of vertex index: the same
// straight line as one segment and as three (extra vertices at x = 100, 200),
// same object stream, same step. Every boil bucket point (s = 15k) lies on the
// line y = 0 and is displaced straight along y, so its y must be identical.
static void test_vertex_density_independent(void) {
    an_scene *s1 = an_scene_create(&(an_scene_desc){ .seed = 5 });
    an_scene *s2 = an_scene_create(&(an_scene_desc){ .seed = 5 });
    an_obj *a = an_stroke(s1, &(an_stroke_opts){ .name = "line", .segs = (an_seg[]){ AN_MOVE(0, 0), AN_LINE(300, 0) }, .seg_count = 2 });
    an_obj *b = an_stroke(s2, &(an_stroke_opts){ .name = "line",
        .segs = (an_seg[]){ AN_MOVE(0, 0), AN_LINE(100, 0), AN_LINE(200, 0), AN_LINE(300, 0) }, .seg_count = 4 });
    CHECK(a->rng_key == b->rng_key);
    const ani_boil boil = { .amp = 3.0f, .spacing = 15.0f, .seed = 5, .stream = a->rng_key, .step = 2 };
    ani_mesh m;
    ani_mesh_init(&m);
    ani_rib_log la = { 0 }, lb = { 0 };
    ani_stroke_in in = { .path = a->path, .world = { 1, 0, 0, 1, 0, 0 }, .width = 6, .progress = 1, .color = AN_BLACK, .boil = boil };
    CHECK(ani_stroke_emit(&m, &in, &la));
    in.path = b->path;
    m.count = 0;
    CHECK(ani_stroke_emit(&m, &in, &lb));
    int compared = 0, bad = 0;
    for (int i = 0; i < la.count; i++) {
        if (fmodf(la.ribs[i].s, 15.0f) != 0.0f || la.ribs[i].s == 0.0f) continue;
        for (int j = 0; j < lb.count; j++) {
            if (lb.ribs[j].s == la.ribs[i].s) {
                compared++;
                bad += la.ribs[i].p.y != lb.ribs[j].p.y;
                break;
            }
        }
    }
    printf("  vertex density: %d bucket points compared (1 vs 3 segments), %d differ\n", compared, bad);
    CHECK(compared >= 18 && bad == 0);
    free(la.ribs);
    free(lb.ribs);
    ani_mesh_free(&m);
    an_scene_destroy(s1);
    an_scene_destroy(s2);
}

int main(void) {
    test_vertex_density_independent();
    test_chaos_zero_identical();
    test_no_crawl();
    test_12hz();
    test_keying_and_shape();
    return check_done("test_boil");
}
