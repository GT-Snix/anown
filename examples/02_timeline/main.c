// 02_timeline: a bubble-sort pass, scripted with the timeline.
//
//   ./build/examples/02_timeline                     preview (Space pauses, arrows step)
//   ./build/examples/02_timeline --export out.mp4    the whole timeline, frames 0..L
//
// Shows: a drop-in on springs started with play_async (heights jittered from
// each box's own seeded stream), par color fades in OKLab, spring swaps, an
// async pointer, and a setter as an instant change at the cursor.
#include "anown.h"

#include <stdio.h>
#include <string.h>

#define YELLOW AN_HEX(0xFFE800)
#define PINK   AN_HEX(0xFF48B0)
#define GREEN  AN_HEX(0x00A95C)
#define INK    AN_HEX(0x1A1A1A)

enum { N = 7 };
static const float GAP = 150.0f, FLOOR_Y = 650.0f, POINTER_Y = 730.0f;

static float slot_x(int i) { return 960.0f + ((float)i - (N - 1) / 2.0f) * GAP; }

int main(int argc, char **argv) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .fps = 60, .seed = 2026 });
    if (!s) return 1;

    // ---- drop in ----
    const int values[N] = { 5, 2, 7, 1, 6, 3, 4 };
    an_obj *box[N];
    for (int i = 0; i < N; i++) {
        char name[16];
        snprintf(name, sizeof name, "box%d", values[i]); // named: stable random stream
        const float h = 60.0f + 40.0f * (float)values[i];
        box[i] = an_rect(s, &(an_rect_opts){
            .name = name, .pos = { slot_x(i), 0 }, .size = { 110, h },
            .fill = YELLOW, .hidden = true,
        });
        // each box starts at its own random height (script-time randomness)
        an_obj_set_pos(box[i], (an_vec2){ slot_x(i), an_rng_range(an_obj_rng(box[i]), -200.0f, 100.0f) });
        // fade in and drop together, then start the next box 80 ms later
        an_play_async(s, an_fade_to(box[i], 1.0f, &(an_anim_opts){ .duration = 0.2f }));
        an_play_async(s, an_move_to(box[i], (an_vec2){ slot_x(i), FLOOR_Y - h / 2.0f },
                                    &(an_anim_opts){ .ease = an_spring(0.45f, 0.7f) }));
        an_wait(s, 0.08f);
    }
    an_wait(s, 0.6f);

    an_obj *pointer = an_circle(s, &(an_circle_opts){
        .pos = { slot_x(0) + GAP / 2, POINTER_Y }, .radius = 18, .fill = INK, .hidden = true });
    an_play(s, an_fade_to(pointer, 1, &(an_anim_opts){ .duration = 0.3f }));

    // ---- one bubble-sort pass ----
    int order[N]; // order[slot] = box index
    for (int i = 0; i < N; i++) order[i] = i;
    for (int j = 0; j < N - 1; j++) {
        an_obj *a = box[order[j]], *b = box[order[j + 1]];
        // the pointer glides on its own track while the pair highlights
        an_play_async(s, an_move_to(pointer, (an_vec2){ slot_x(j) + GAP / 2, POINTER_Y },
                                    &(an_anim_opts){ .ease = an_spring(1.0f, 0.4f) }));
        an_par(s, 2, (an_anim[]){ an_fill_to(a, PINK, &(an_anim_opts){ .duration = 0.25f }),
                                  an_fill_to(b, PINK, &(an_anim_opts){ .duration = 0.25f }) });
        if (values[order[j]] > values[order[j + 1]]) {
            an_par(s, 2, (an_anim[]){
                an_move_by(a, (an_vec2){ GAP, 0 }, &(an_anim_opts){ .ease = an_spring(0.6f, 0.45f) }),
                an_move_by(b, (an_vec2){ -GAP, 0 }, &(an_anim_opts){ .ease = an_spring(0.6f, 0.45f) }),
            });
            const int t = order[j];
            order[j] = order[j + 1];
            order[j + 1] = t;
        } else {
            an_wait(s, 0.25f);
        }
        an_par(s, 2, (an_anim[]){ an_fill_to(a, YELLOW, &(an_anim_opts){ .duration = 0.25f }),
                                  an_fill_to(b, YELLOW, &(an_anim_opts){ .duration = 0.25f }) });
    }

    // ---- the largest value is in place ----
    an_obj *largest = box[order[N - 1]];
    an_obj_set_fill(largest, GREEN); // instant, at the cursor
    an_par(s, 2, (an_anim[]){
        an_fade_to(pointer, 0, &(an_anim_opts){ .duration = 0.3f }),
        an_scale_to(largest, (an_vec2){ 1.15f, 1.15f }, &(an_anim_opts){ .ease = an_spring(0.3f, 0.4f) }),
    });
    an_play(s, an_scale_to(largest, (an_vec2){ 1, 1 }, &(an_anim_opts){ .ease = an_spring(0.5f, 0.4f) }));
    an_wait(s, 0.5f);

    printf("timeline: %.3f s, frames 0..%lld\n", an_scene_time(s), (long long)an_scene_length(s));
    if (argc >= 3 && strcmp(argv[1], "--export") == 0) {
        const int64_t n = an_scene_export(s, &(an_export_desc){ .path = argv[2] });
        an_scene_destroy(s);
        if (n > 0) printf("wrote %s (%lld frames)\n", argv[2], (long long)n);
        return n > 0 ? 0 : 1;
    }
    an_scene_preview(s); // does not return on macOS
    return 0;
}
