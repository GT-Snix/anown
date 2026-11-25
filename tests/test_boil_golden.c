// Golden hashes of the full triangle list (positions and colors, in order) for a
// fixed scene with boil on, at frames 0, 7, 40, 180 and 24, 30, 60 fps.
//
// Recorded BEFORE the boil performance work (one hash per bucket, displacing in
// the gather pass, reused scratch buffers) so that work can be shown to change
// nothing: the output must match these hashes bit for bit.
//
//   ./build/tests/test_boil_golden --print   prints the current hashes
#include "anown.h"
#include "check.h"
#include "tess_internal.h"

#include <string.h>

static const an_seg WAVE[] = { AN_MOVE(100, 300), AN_CUBIC(400, 50, 700, 550, 1000, 300), AN_LINE(1300, 420),
                               AN_CUBIC(1500, 500, 1700, 200, 1800, 280) };
static const float PRESSURE[4] = { 0.5f, 1.2f, 0.7f, 1.0f };

static an_scene *golden_scene(int fps) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .fps = fps, .seed = 2026, .style = { .shadow = true } });
    an_obj *wave = an_stroke(s, &(an_stroke_opts){ .name = "wave", .segs = WAVE, .seg_count = 4, .stroke = AN_HEX(0x16161A),
                                                   .stroke_width = 9, .undrawn = true,
                                                   .brush = { .taper_in = 30, .taper_out = 80, .overshoot = 0.6f,
                                                              .pressure = PRESSURE, .pressure_count = 4 } });
    an_obj *box = an_rect(s, &(an_rect_opts){ .pos = { 420, 720 }, .size = { 300, 200 }, .fill = AN_HEX(0xFF2E88),
                                              .stroke = AN_HEX(0x16161A), .stroke_width = 8, .undrawn = true,
                                              .brush = { .taper_in = 20, .taper_out = 40, .overshoot = 0.7f } });
    an_obj *g = an_group(s, &(an_group_opts){ .pos = { 1250, 760 }, .rotation = 0.35f, .scale = { 1.3f, 1.3f } });
    an_obj *ring = an_circle(s, &(an_circle_opts){ .parent = g, .radius = 110, .fill = AN_HEX(0x00C2FF),
                                                   .stroke = AN_HEX(0x16161A), .stroke_width = 7, .shadow = AN_OFF });
    an_obj *arrow = an_arrow(s, &(an_arrow_opts){ .from = { 650, 950 }, .to = { 1050, 840 }, .stroke = AN_HEX(0x16161A),
                                                  .stroke_width = 6, .undrawn = true });
    an_obj *dots = an_stroke(s, &(an_stroke_opts){ .segs = (an_seg[]){ AN_MOVE(1500, 150), AN_LINE(1500, 150), AN_MOVE(1560, 150),
                                                                       AN_LINE(1700, 160), AN_CLOSE },
                                                   .seg_count = 5, .stroke = AN_HEX(0x16161A), .stroke_width = 12 });
    an_play(s, an_draw(wave, &(an_anim_opts){ .duration = 2.0f, .ease = AN_EASE_LINEAR }));
    an_par(s, 2, (an_anim[]){ an_draw(box, NULL), an_draw(arrow, &(an_anim_opts){ .duration = 1.5f }) });
    an_play(s, an_rotate_by(g, 0.8f, &(an_anim_opts){ .ease = an_spring(0.4f, 0.6f) }));
    an_play_async(s, an_fade_to(ring, 0.6f, NULL));
    an_set_chaos(dots, 0.5f);
    an_wait(s, 3.0f);
    (void)ring;
    return s;
}

// Silhouette vertices (the shadow cut, added after the goldens were recorded) are
// skipped: each object appends them as one range after its other vertices, so
// the remaining sequence is exactly what was recorded. Matching proves the cut
// changed no other vertex.
static unsigned char g_skip[1 << 20];

static uint64_t mesh_hash(const ani_mesh *m) {
    uint64_t h = 0xcbf29ce484222325ull;
    CHECK(m->count <= (int)sizeof g_skip);
    memset(g_skip, 0, (size_t)m->count);
    int n = m->count;
    for (int c = 0; c < m->ncmds; c++) {
        if (m->cmds[c].paint != ANI_PAINT_SILHOUETTE) continue;
        for (int i = m->cmds[c].first; i < m->cmds[c].first + m->cmds[c].count; i++) {
            n -= !g_skip[i];
            g_skip[i] = 1;
        }
    }
    const unsigned char *pc = (const unsigned char *)&n;
    for (size_t k = 0; k < sizeof n; k++) h = (h ^ pc[k]) * 0x100000001b3ull;
    for (int i = 0; i < m->count; i++) {
        if (g_skip[i]) continue;
        const float f[6] = { m->v[i].x, m->v[i].y, m->v[i].r, m->v[i].g, m->v[i].b, m->v[i].a };
        const unsigned char *p = (const unsigned char *)f;
        for (size_t k = 0; k < sizeof f; k++) h = (h ^ p[k]) * 0x100000001b3ull;
    }
    return h;
}

static const int FPS[3] = { 24, 30, 60 };
static const int64_t FRAMES[4] = { 0, 7, 40, 180 };

// Recorded 2026-11-24, before the boil performance work; re-recorded 2026-11-25
// for an intended change: unnamed objects' boil keys now come from their place in
// the tree, not a global creation counter (the "wave" is named, so only the
// box, ring, arrow and dots changed). The previous values were
//   24 fps: 41a4a9e780755266 15995d0add2cd625 e5ae03518c78f1fc e21d1a4ec41c2560
//   30 fps: 41a4a9e780755266 813fe969273ecb55 9e6868d359db396d 5c7d64decc3b0a54
//   60 fps: 41a4a9e780755266 9fc4b82bb597159f f27e9ebfcd886ae5 5eabeee737fe5af0
static const uint64_t GOLDEN[3][4] = {
    { 0x7b1059db9b90e498ull, 0xc47b5adac0c601e3ull, 0x4cac7e5969c822e1ull, 0xd2813cfcc840968cull }, // 24 fps
    { 0x7b1059db9b90e498ull, 0xe1eda6b561fc02daull, 0xd18b8f17d5636184ull, 0x35e2217eaa7a4175ull }, // 30 fps
    { 0x7b1059db9b90e498ull, 0xa89f6a7f0a1581e0ull, 0x9ef0af1824353195ull, 0xd1b3fb3d8e43cbb6ull }, // 60 fps
};

int main(int argc, char **argv) {
    const bool print = argc > 1 && strcmp(argv[1], "--print") == 0;
    ani_mesh m;
    ani_mesh_init(&m);
    int bad = 0;
    for (int f = 0; f < 3; f++) {
        an_scene *s = golden_scene(FPS[f]);
        if (print) printf("    { ");
        for (int k = 0; k < 4; k++) {
            CHECK(ani_tess_scene(&m, s, FRAMES[k]));
            const uint64_t h = mesh_hash(&m);
            if (print) printf("0x%016llxull%s", (unsigned long long)h, k < 3 ? ", " : "");
            if (!print && h != GOLDEN[f][k]) {
                fprintf(stderr, "  %d fps frame %lld: hash %016llx, golden %016llx (%d vertices)\n", FPS[f],
                        (long long)FRAMES[k], (unsigned long long)h, (unsigned long long)GOLDEN[f][k], m.count);
                bad++;
            }
        }
        if (print) printf(" }, // %d fps\n", FPS[f]);
        an_scene_destroy(s);
    }
    ani_mesh_free(&m);
    if (print) return 0;
    CHECK(bad == 0);
    return check_done("test_boil_golden");
}
