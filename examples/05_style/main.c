// 05_style: 3 seconds of the 5B look. Riot palette on grained paper with a
// vignette, riso misregistration on everything that has ink, hard shadows (scene
// default on, one object opted out), tapered charcoal ink drawing on, and a calm
// object (chaos 0) next to a loud one.
//
//   ./build/examples/05_style                       preview
//   ./build/examples/05_style --export out.mp4      the 3 s timeline
//   ./build/examples/05_style --frames DIR [STEP]   PNG of every STEP-th frame
//                                                   (default 5: 12 fps, the grain's clock)
#include "anown.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const an_palette *pal = &AN_PALETTE_RIOT;
    an_scene *s = an_scene_create(&(an_scene_desc){ .fps = 60, .seed = 7, .style = { .palette = pal, .shadow = true } });
    if (!s) return 1;
    const an_brush ink = { .taper_in = 24, .taper_out = 60, .overshoot = 0.6f };

    // a row of array cells: magenta fills, charcoal ink, riso and shadow on
    an_obj *cells[5];
    an_anim draws[5];
    for (int i = 0; i < 5; i++) {
        cells[i] = an_rect(s, &(an_rect_opts){
            .pos = { 520.0f + 220.0f * (float)i, 330 }, .size = { 170, 170 }, .fill = i == 2 ? pal->c : pal->b,
            .stroke = pal->ink, .stroke_width = 10, .brush = ink, .undrawn = true });
        draws[i] = an_draw(cells[i], &(an_anim_opts){ .duration = 0.7f });
    }
    // a cyan circle with ink, and an arrow pointing at the middle cell
    an_obj *ring = an_circle(s, &(an_circle_opts){ .pos = { 400, 760 }, .radius = 150, .fill = pal->a,
                                                   .stroke = pal->ink, .stroke_width = 10, .brush = ink, .undrawn = true });
    an_obj *arrow = an_arrow(s, &(an_arrow_opts){ .from = { 600, 760 }, .to = { 930, 460 }, .head = 44,
                                                  .stroke = pal->ink, .stroke_width = 9, .brush = ink, .undrawn = true });
    // a calm label plate: chaos 0, so no riso or shadow (legibility)
    an_obj *plate = an_rect(s, &(an_rect_opts){ .pos = { 1380, 760 }, .size = { 520, 200 }, .fill = pal->paper,
                                                .stroke = pal->ink, .stroke_width = 6, .shadow = AN_OFF, .hidden = true });
    an_set_chaos(plate, 0.0f);
    // a fill-only lime blob forced to misregister
    an_obj *blob = an_circle(s, &(an_circle_opts){ .pos = { 1380, 760 }, .radius = 60, .fill = pal->c, .riso = AN_ON, .hidden = true });

    an_stagger(s, 5, draws, 0.12f);
    an_par(s, 2, (an_anim[]){ an_draw(ring, &(an_anim_opts){ .duration = 0.8f }),
                              an_draw(arrow, &(an_anim_opts){ .duration = 0.8f }) });
    an_par(s, 2, (an_anim[]){ an_fade_to(plate, 1, &(an_anim_opts){ .duration = 0.3f }),
                              an_fade_to(blob, 1, &(an_anim_opts){ .duration = 0.3f }) });
    an_play(s, an_move_by(cells[2], (an_vec2){ 0, -40 }, &(an_anim_opts){ .duration = 0.4f }));
    const double left = 3.0 - an_scene_time(s);
    if (left > 0) an_wait(s, (float)left);

    printf("timeline %.2f s, frames 0..%lld\n", an_scene_time(s), (long long)an_scene_length(s));
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
            snprintf(path, sizeof path, "%s/style_%04lld.png", argv[2], (long long)f);
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
