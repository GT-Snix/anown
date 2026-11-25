// 08_text: a title and a short code snippet (docs/text.md).
//   - the title draws on letter by letter (tag-draw): the outline like a marker,
//     each letter's riso fill popping in as its outline closes; then a wave runs
//     through the letters (per-letter stagger) and one spins about its pivot
//   - a subtitle fades in per letter
//   - the code block's box draws on, its lines type in (typewriter), and the
//     highlight bar steps through the partition loop. The code is calm (its size
//     factor puts chaos at 0 at 30 px); the box and bars boil.
//
//   ./build/examples/08_text                       preview
//   ./build/examples/08_text --export out/text.mp4  the whole timeline
//   ./build/examples/08_text --frames DIR [STEP]    PNG every STEP-th frame (default 5)
#include "anown.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const an_palette *pal = &AN_PALETTE_RIOT;
    an_scene *s = an_scene_create(&(an_scene_desc){ .fps = 60, .seed = 8, .style = { .palette = pal, .shadow = true } });
    if (!s) return 1;

    // title: drawn on, then hops
    an_obj *title = an_text(s, &(an_text_opts){ .text = "QUICKSORT", .pos = { 960, 230 }, .size = 190, .align = AN_ALIGN_CENTER,
                                                 .fill = pal->a, .stroke = pal->ink, .stroke_width = 9, .undrawn = true });
    an_obj *sub = an_text(s, &(an_text_opts){ .text = "partition, one swap at a time", .pos = { 960, 330 }, .size = 54,
                                               .align = AN_ALIGN_CENTER, .fill = pal->b, .letters_hidden = true });
    static const char *code =
        "int partition(int *a, int lo, int hi) {\n"
        "    int pivot = a[hi], i = lo - 1;\n"
        "    for (int j = lo; j < hi; j++) {\n"
        "        if (a[j] < pivot) swap(a, ++i, j);\n"
        "    }\n"
        "    swap(a, i + 1, hi);\n"
        "    return i + 1;\n"
        "}";
    an_obj *blk = an_code(s, &(an_code_opts){ .pos = { 420, 440 }, .code = code, .line_numbers = true, .hidden = true });

    an_wait(s, 0.3f);
    an_play(s, an_draw(title, &(an_anim_opts){ .duration = 2.0f, .ease = AN_EASE_LINEAR }));
    {
        const int n = an_text_length(sub);
        an_anim fades[64];
        for (int i = 0; i < n && i < 64; i++) fades[i] = an_fade_to(an_text_glyph(sub, i), 1.0f, &(an_anim_opts){ .duration = 0.3f });
        an_stagger(s, n < 64 ? n : 64, fades, 0.025f);
    }
    // the title's letters hop, one after another, and the 'S' spins about its pivot
    {
        const int n = an_text_length(title);
        an_anim up[16], down[16];
        for (int i = 0; i < n; i++) {
            up[i] = an_move_by(an_text_glyph(title, i), (an_vec2){ 0, -36 }, &(an_anim_opts){ .duration = 0.18f });
            down[i] = an_move_by(an_text_glyph(title, i), (an_vec2){ 0, 36 }, &(an_anim_opts){ .duration = 0.5f, .ease = an_spring(0.35f, 0.35f) });
        }
        // a wave up, then a wave back down (springy)
        an_stagger(s, n, up, 0.06f);
        an_stagger(s, n, down, 0.06f);
        an_play(s, an_rotate_by(an_text_glyph(title, 5), 6.2831853f, &(an_anim_opts){ .duration = 0.8f }));
    }

    // code: the box appears, lines type in, the highlight steps through the loop
    an_obj_set_opacity(blk, 1.0f);
    for (int l = 1; l <= 8; l++) an_obj_set_draw(an_code_text(blk, l), 0.0f);
    {
        an_anim typing[8];
        for (int l = 1; l <= 8; l++) typing[l - 1] = an_draw(an_code_text(blk, l), &(an_anim_opts){ .duration = 0.45f, .ease = AN_EASE_LINEAR });
        an_stagger(s, 8, typing, 0.3f);
    }
    an_wait(s, 0.3f);
    const int steps[5] = { 2, 3, 4, 3, 6 };
    for (int k = 0; k < 5; k++) {
        an_obj *bar = an_code_line(blk, steps[k]);
        an_play(s, an_fade_to(bar, 1.0f, &(an_anim_opts){ .duration = 0.2f }));
        an_wait(s, 0.45f);
        an_play(s, an_fade_to(bar, 0.0f, &(an_anim_opts){ .duration = 0.2f }));
    }
    an_play(s, an_fade_to(an_code_line(blk, 7), 1.0f, &(an_anim_opts){ .duration = 0.2f }));
    an_wait(s, 1.0f);

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
            snprintf(path, sizeof path, "%s/text_%04lld.png", argv[2], (long long)f);
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
