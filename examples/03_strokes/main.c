// 03_strokes: a rect, a circle and an arrow drawing on with tapered charcoal
// ink strokes, then an underline with a pressure curve.
//
//   ./build/examples/03_strokes                      preview
//   ./build/examples/03_strokes --export out.mp4     the whole timeline
//   ./build/examples/03_strokes --png N out.png      one frame
//
// Prints the nozzle (pen tip) position and speed at a few frames: it's a pure
// function of (path, frame), meant for drips and spatter later.
#include "anown.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHARCOAL AN_HEX(0x16161A)
#define YELLOW   AN_HEX(0xFFE800)
#define PINK     AN_HEX(0xFF48B0)

int main(int argc, char **argv) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .fps = 60, .seed = 5 });
    if (!s) return 1;

    // one brush for everything: the pen lands thin, leaves thinner, and runs a
    // little past sharp corners
    const an_brush ink = { .taper_in = 24, .taper_out = 70, .overshoot = 0.7f };

    // a highlight behind the rect, faded in after the outline is drawn
    an_obj *hl = an_rect(s, &(an_rect_opts){ .pos = { 480, 470 }, .size = { 380, 260 }, .fill = YELLOW, .hidden = true });

    an_obj *box = an_rect(s, &(an_rect_opts){
        .pos = { 470, 460 }, .size = { 380, 260 }, .no_fill = true, .undrawn = true,
        .stroke = CHARCOAL, .stroke_width = 12, .brush = ink,
    });
    an_obj *ring = an_circle(s, &(an_circle_opts){
        .pos = { 1430, 460 }, .radius = 150, .no_fill = true, .undrawn = true,
        .stroke = CHARCOAL, .stroke_width = 12, .brush = ink,
    });
    an_obj *arrow = an_arrow(s, &(an_arrow_opts){
        .from = { 720, 470 }, .to = { 1230, 460 }, .head = 46, .undrawn = true,
        .stroke = CHARCOAL, .stroke_width = 10, .brush = ink,
    });
    static const float pressure[6] = { 0.4f, 1.0f, 0.7f, 1.2f, 0.9f, 0.5f };
    an_obj *underline = an_stroke(s, &(an_stroke_opts){
        .segs = (an_seg[]){ AN_MOVE(330, 760), AN_CUBIC(700, 730, 1150, 800, 1600, 750) }, .seg_count = 2,
        .undrawn = true, .stroke = PINK, .stroke_width = 18,
        .brush = { .taper_in = 40, .taper_out = 120, .pressure = pressure, .pressure_count = 6 },
    });

    an_play(s, an_draw(box, &(an_anim_opts){ .duration = 1.4f }));
    an_play(s, an_fade_to(hl, 1, &(an_anim_opts){ .duration = 0.3f }));
    an_play(s, an_draw(arrow, &(an_anim_opts){ .duration = 0.9f }));
    an_play(s, an_draw(ring, &(an_anim_opts){ .duration = 1.2f }));
    an_play(s, an_draw(underline, &(an_anim_opts){ .duration = 0.8f, .ease = AN_EASE_LINEAR }));
    an_play(s, an_stroke_width_to(ring, 20, &(an_anim_opts){ .ease = an_spring(0.35f, 0.5f) }));
    an_wait(s, 0.6f);

    printf("timeline %.2f s, frames 0..%lld\n", an_scene_time(s), (long long)an_scene_length(s));
    const int64_t probe[5] = { 20, 42, 110, 150, 225 };
    an_obj *who[5] = { box, box, arrow, ring, underline };
    for (int i = 0; i < 5; i++) {
        an_seek(s, probe[i]);
        const an_nozzle nz = an_obj_nozzle(who[i]);
        printf("  frame %3lld nozzle: %s pos (%.1f, %.1f) dir (%.2f, %.2f) speed %.0f px/s width %.1f\n",
               (long long)probe[i], nz.active ? "active" : "idle  ", nz.pos.x, nz.pos.y, nz.dir.x, nz.dir.y,
               nz.speed, nz.width);
    }

    int rc = 0;
    if (argc >= 3 && strcmp(argv[1], "--export") == 0) {
        const int64_t n = an_scene_export(s, &(an_export_desc){ .path = argv[2] });
        if (n > 0) printf("wrote %s (%lld frames)\n", argv[2], (long long)n);
        rc = n > 0 ? 0 : 1;
    } else if (argc >= 4 && strcmp(argv[1], "--png") == 0) {
        const bool ok = an_scene_save_png(s, atoll(argv[2]), argv[3]);
        if (ok) printf("wrote %s\n", argv[3]);
        rc = ok ? 0 : 1;
    } else {
        an_scene_preview(s); // does not return on macOS
    }
    an_scene_destroy(s);
    return rc;
}
