// 06_look: the chaos dial from 0 to 1 on text-free content (shapes only; text
// isn't drawn yet). The scene's chaos holds at 0, ramps to 1 over 3 s, then
// holds. As it rises, boil, riso misregistration, hard shadows, paper grain and
// the vignette all come up together. A "legend" group is pinned at chaos 0 with
// the per-object multiplier, the legibility control for key content.
//
//   ./build/examples/06_look                      preview
//   ./build/examples/06_look --export out.mp4     the whole timeline
//   ./build/examples/06_look --frames DIR [STEP]  PNG every STEP-th frame (default 5: 12 fps)
#include "anown.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const an_palette *pal = &AN_PALETTE_RIOT;
    an_scene *s = an_scene_create(&(an_scene_desc){ .fps = 60, .seed = 11, .style = { .palette = pal, .shadow = true } });
    if (!s) return 1;
    an_set_chaos(s, 0.0f);
    const an_brush ink = { .taper_in = 18, .taper_out = 40, .overshoot = 0.6f };
    const an_color INK = pal->ink;

    // an array of cells: solid, halftone at two angles, hatch
    const an_pattern pats[5] = {
        { 0 },
        { .kind = AN_PATTERN_HALFTONE, .size = 18, .density = 0.45f },
        { .kind = AN_PATTERN_HALFTONE, .size = 12, .density = 0.6f, .angle = 0.785398f },
        { .kind = AN_PATTERN_HATCH, .size = 14, .density = 0.35f, .angle = -0.6f },
        { .kind = AN_PATTERN_HATCH, .size = 10, .density = 0.5f, .angle = 0.6f },
    };
    const an_color fills[5] = { pal->b, pal->a, pal->b, pal->c, pal->a };
    for (int i = 0; i < 5; i++) {
        an_rect(s, &(an_rect_opts){ .pos = { 360.0f + 200.0f * (float)i, 260 }, .size = { 160, 160 }, .fill = fills[i],
                                    .stroke = INK, .stroke_width = 9, .brush = ink, .pattern = pats[i] });
    }

    // a small graph: edges as strokes, halftone and solid nodes
    const an_vec2 nodes[4] = { { 360, 640 }, { 620, 520 }, { 620, 800 }, { 880, 660 } };
    const int edges[5][2] = { { 0, 1 }, { 0, 2 }, { 1, 3 }, { 2, 3 }, { 1, 2 } };
    for (int e = 0; e < 5; e++) {
        const an_vec2 a = nodes[edges[e][0]], b = nodes[edges[e][1]];
        an_line(s, &(an_line_opts){ .from = a, .to = b, .stroke = INK, .stroke_width = 6, .brush = ink, .shadow = AN_OFF });
    }
    for (int n = 0; n < 4; n++) {
        an_circle(s, &(an_circle_opts){ .pos = nodes[n], .radius = 58, .fill = n == 3 ? pal->c : pal->a, .stroke = INK,
                                        .stroke_width = 8, .brush = ink, .z = 1,
                                        .pattern = n % 2 ? (an_pattern){ .kind = AN_PATTERN_HALFTONE, .size = 14, .density = 0.55f }
                                                         : (an_pattern){ 0 } });
    }

    // a wave and an arrow
    an_stroke(s, &(an_stroke_opts){ .segs = (an_seg[]){ AN_MOVE(1080, 560), AN_CUBIC(1230, 440, 1380, 720, 1530, 590),
                                                        AN_CUBIC(1620, 510, 1700, 560, 1760, 520) },
                                    .seg_count = 3, .stroke = pal->b, .stroke_width = 14,
                                    .brush = { .taper_in = 40, .taper_out = 90 } });
    an_arrow(s, &(an_arrow_opts){ .from = { 1100, 820 }, .to = { 1480, 700 }, .head = 40, .stroke = INK, .stroke_width = 9, .brush = ink });

    // the legend: pinned calm (multiplier 0), however loud the scene gets
    an_obj *legend = an_group(s, &(an_group_opts){ .pos = { 1600, 860 } });
    an_rect(s, &(an_rect_opts){ .parent = legend, .size = { 360, 180 }, .fill = pal->paper, .stroke = INK, .stroke_width = 4 });
    for (int i = 0; i < 3; i++) {
        an_rect(s, &(an_rect_opts){ .parent = legend, .pos = { -110.0f + 110.0f * (float)i, 0 }, .size = { 70, 70 },
                                    .fill = i == 0 ? pal->a : (i == 1 ? pal->b : pal->c), .stroke = INK, .stroke_width = 3 });
    }
    an_set_chaos(legend, 0.0f);

    // the dial: hold 0, ramp to 1 over 3 s, hold
    an_wait(s, 0.5f);
    an_play(s, an_scene_chaos_to(s, 1.0f, &(an_anim_opts){ .duration = 3.0f, .ease = AN_EASE_LINEAR }));
    an_wait(s, 1.0f);

    printf("timeline %.2f s, frames 0..%lld (chaos 0 until 0.5 s, 1 from 3.5 s)\n", an_scene_time(s),
           (long long)an_scene_length(s));
    int rc = 0;
    if (argc >= 3 && strcmp(argv[1], "--export") == 0) {
        const int64_t n = an_scene_export(s, &(an_export_desc){ .path = argv[2] });
        if (n > 0) printf("wrote %s (%lld frames)\n", argv[2], (long long)n);
        rc = n > 0 ? 0 : 1;
    } else if (argc >= 3 && strcmp(argv[1], "--frames") == 0) {
        const int step = argc >= 4 ? atoi(argv[3]) : 5;
        int count = 0;
        for (int64_t f = 0; f <= an_scene_length(s); f += step > 0 ? step : 5) {
            char path[512];
            snprintf(path, sizeof path, "%s/look_%04lld.png", argv[2], (long long)f);
            if (!an_scene_save_png(s, f, path)) {
                rc = 1;
                break;
            }
            count++;
        }
        printf("wrote %d PNG frames to %s\n", count, argv[2]);
    } else {
        an_scene_preview(s); // does not return on macOS
    }
    an_scene_destroy(s);
    return rc;
}
