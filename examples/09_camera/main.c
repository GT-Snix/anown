// 09_camera: the camera and the first verbs (docs/camera.md).
//   - the title slaps in letter by letter (each from 2.5x with a tilt); the last
//     letter's impact shakes the camera
//   - a graph: one node pops in, another squash-stretches when an edge reaches it
//   - the camera pans and zooms to 2.5x onto a node, holds, and comes back
//   - a second slap with a bigger shake to end
//
//   ./build/examples/09_camera                          preview
//   ./build/examples/09_camera --export out/camera.mp4   the whole timeline (share mode)
//   ./build/examples/09_camera --frames DIR [STEP]       PNG every STEP-th frame (default 5)
#include "anown.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const an_palette *pal = &AN_PALETTE_RIOT;
    an_scene *s = an_scene_create(&(an_scene_desc){ .fps = 60, .seed = 9, .style = { .palette = pal, .shadow = true } });
    if (!s) return 1;
    const an_color INK = pal->ink;
    const an_brush ink = { .taper_in = 18, .taper_out = 40, .overshoot = 0.6f };

    // the title: letters hidden until their slap
    an_obj *title = an_text(s, &(an_text_opts){ .text = "SLAP!", .pos = { 960, 250 }, .size = 210,
                                                 .align = AN_ALIGN_CENTER, .anchor = AN_ANCHOR_MIDDLE,
                                                 .fill = pal->b, .stroke = INK, .stroke_width = 9,
                                                 .letters_hidden = true });
    // a small graph below
    const an_vec2 nodes[3] = { { 560, 700 }, { 960, 820 }, { 1360, 680 } };
    an_obj *edges[2];
    for (int e = 0; e < 2; e++) {
        edges[e] = an_line(s, &(an_line_opts){ .from = nodes[e], .to = nodes[e + 1], .stroke = INK, .stroke_width = 7,
                                                .brush = ink, .undrawn = true, .shadow = AN_OFF });
    }
    an_obj *dots[3];
    for (int n = 0; n < 3; n++) {
        dots[n] = an_circle(s, &(an_circle_opts){ .pos = nodes[n], .radius = 64, .z = 1, .fill = n == 1 ? pal->c : pal->a,
                                                  .stroke = INK, .stroke_width = 8, .brush = ink, .hidden = n == 0,
                                                  .pattern = n == 2 ? (an_pattern){ .kind = AN_PATTERN_HALFTONE, .size = 14, .density = 0.55f }
                                                                    : (an_pattern){ 0 },
                                                  .paper_under = n == 2 }); // the edge mustn't show through the dots
    }

    an_wait(s, 0.4f);
    // slap the letters in, one after another; the last one shakes the camera
    const int n = an_text_length(title);
    for (int i = 0; i < n; i++) {
        an_obj *g = an_text_glyph(title, i);
        an_obj_set_opacity(g, 1.0f);
        an_play_async(s, an_slap(g, &(an_verb_opts){ .impact_shake = i == n - 1 ? 18.0f : 0.0f }));
        an_wait(s, 0.14f);
    }
    an_wait(s, 0.6f);

    // the graph: node 0 pops in, the edges draw, node 2 squashes on arrival
    an_obj_set_opacity(dots[0], 1.0f);
    an_play(s, an_pop(dots[0], NULL));
    an_play(s, an_draw(edges[0], &(an_anim_opts){ .duration = 0.5f }));
    an_play_async(s, an_squash_stretch(dots[1], NULL));
    an_play(s, an_draw(edges[1], &(an_anim_opts){ .duration = 0.5f }));
    an_play(s, an_squash_stretch(dots[2], &(an_verb_opts){ .amount = 0.4f }));

    // the camera: pan and zoom onto node 2, hold, come back
    an_par(s, 2, (an_anim[]){ an_camera_move_to(s, nodes[2], &(an_anim_opts){ .duration = 1.1f }),
                              an_camera_zoom_to(s, 2.5f, &(an_anim_opts){ .duration = 1.1f }) });
    an_play(s, an_squash_stretch(dots[2], &(an_verb_opts){ .amount = 0.25f }));
    an_wait(s, 0.4f);
    an_par(s, 2, (an_anim[]){ an_camera_move_to(s, (an_vec2){ 960, 540 }, &(an_anim_opts){ .duration = 1.0f }),
                              an_camera_zoom_to(s, 1.0f, &(an_anim_opts){ .duration = 1.0f }) });

    // the title again: one big slap and shake
    an_play(s, an_slap(title, &(an_verb_opts){ .amount = 1.8f, .impact_shake = 26.0f, .impact_shake_duration = 0.7f }));
    an_wait(s, 0.9f);

    printf("timeline %.2f s, frames 0..%lld\n", an_scene_time(s), (long long)an_scene_length(s));
    int rc = 0;
    if (argc >= 3 && strcmp(argv[1], "--export") == 0) {
        const int64_t f = an_scene_export(s, &(an_export_desc){ .path = argv[2] });
        if (f > 0) printf("wrote %s (%lld frames)\n", argv[2], (long long)f);
        rc = f > 0 ? 0 : 1;
    } else if (argc >= 3 && strcmp(argv[1], "--frames") == 0) {
        const int step = argc >= 4 ? atoi(argv[3]) : 5;
        int count = 0;
        for (int64_t f = 0; f <= an_scene_length(s); f += step > 0 ? step : 5) {
            char path[512];
            snprintf(path, sizeof path, "%s/camera_%04lld.png", argv[2], (long long)f);
            if (!an_scene_save_png(s, f, path)) {
                rc = 1;
                break;
            }
            count++;
        }
        printf("wrote %d PNG frames to %s\n", count, argv[2]);
    } else {
        an_scene_preview(s);
    }
    an_scene_destroy(s);
    return rc;
}
