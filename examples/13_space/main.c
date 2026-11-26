// 13_space: the 3D space, milestone A (docs/space.md). A grid seen straight
// from above (it looks like the 2D plane) tilts back into 3D, the camera orbits
// around three vectors, one of them leaving the floor, and the space takes a 3x3
// matrix. Then the camera dollies in and settles.
//
//   ./build/examples/13_space [--frames DIR] [--every N] [--export FILE.mp4]
#include "anown.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const float PI = 3.14159265358979f;
#define MAGENTA AN_HEX(0xE6007E)
#define CYAN AN_HEX(0x00B4D8)
#define ORANGE AN_HEX(0xFF6A00)

int main(int argc, char **argv) {
    const char *dir = NULL, *mp4 = NULL;
    int every = 30;
    for (int k = 1; k + 1 < argc; k += 2) {
        if (strcmp(argv[k], "--frames") == 0) dir = argv[k + 1];
        else if (strcmp(argv[k], "--every") == 0) every = atoi(argv[k + 1]);
        else if (strcmp(argv[k], "--export") == 0) mp4 = argv[k + 1];
    }
    an_scene *s = an_scene_create(&(an_scene_desc){ .width = 1920, .height = 1080, .fps = 30, .seed = 13,
                                                    .style = { .grain_mode = AN_GRAIN_STATIC } });
    if (!s) return 1;
    an_obj *sp = an_space(s, &(an_space_opts){ .axes = true, .grid_extent = 6 });

    // three vectors, drawn on: two on the floor, one up out of it
    const an_arrow_opts look = { .stroke_width = 7, .undrawn = true, .riso = AN_OFF, .shadow = AN_OFF };
    an_arrow_opts o = look;
    o.stroke = MAGENTA;
    an_obj *u = an_space_vector(sp, (an_vec3){ 3, 0, 0 }, &o);
    o.stroke = CYAN;
    an_obj *v = an_space_vector(sp, (an_vec3){ 1, 2, 0 }, &o);
    o.stroke = ORANGE;
    an_obj *w = an_space_vector(sp, (an_vec3){ 1, 1, 2.5f }, &o);
    an_obj *tip = an_space_dot(sp, (an_vec3){ 1, 1, 2.5f }, &(an_circle_opts){ .radius = 9, .fill = ORANGE, .hidden = true,
                                                                              .riso = AN_OFF, .shadow = AN_OFF });
    // its shadow on the floor: a dashed-looking drop line from the tip to (1, 1, 0)
    an_obj *drop = an_space_path(sp, (an_vec3[]){ { 1, 1, 2.5f }, { 1, 1, 0 }, { 0, 0, 0 } }, 3, false,
                                 &(an_line_opts){ .stroke = AN_HEX(0x9A948A), .stroke_width = 3, .undrawn = true,
                                                  .riso = AN_OFF, .shadow = AN_OFF });
    an_wait(s, 0.5f);
    an_play(s, an_draw(u, &(an_anim_opts){ .duration = 0.6f }));
    an_play(s, an_draw(v, &(an_anim_opts){ .duration = 0.6f }));
    an_play(s, an_draw(w, &(an_anim_opts){ .duration = 0.6f })); // straight at the camera: a short arrow
    an_wait(s, 0.6f);

    // tilt back: the floor recedes, the orange arrow stands up
    an_play(s, an_space_orbit_to(sp, (an_orbit){ -0.5f, 1.05f, 12 }, &(an_anim_opts){ .duration = 2.5f }));
    an_play_async(s, an_fade_to(tip, 1, &(an_anim_opts){ .duration = 0.4f }));
    an_play(s, an_draw(drop, &(an_anim_opts){ .duration = 0.8f }));
    an_wait(s, 0.5f);

    // a full orbit, rising a little
    an_play(s, an_space_orbit_to(sp, (an_orbit){ -0.5f + 2 * PI, 0.85f, 12 },
                                 &(an_anim_opts){ .duration = 7, .ease = AN_EASE_LINEAR }));
    // the space takes A = [[3, 1, 0], [0, 2, 0], [0, 0, 1]]: the floor shears, z stays
    an_play(s, an_space_matrix_to(sp, (an_mat3){ { 3, 1, 0, 0, 2, 0, 0, 0, 1 } }, &(an_anim_opts){ .duration = 2.5f }));
    an_wait(s, 0.8f);
    // dolly in, then settle low and to the side
    an_play(s, an_space_orbit_to(sp, (an_orbit){ 0.4f, 1.2f, 9 }, &(an_anim_opts){ .duration = 2.5f }));
    an_wait(s, 1.0f);
    printf("13_space: %.2f s, frames 0..%lld\n", an_scene_time(s), (long long)an_scene_length(s));

    int rc = 0;
    if (dir) {
        const int64_t n = an_scene_save_frames(s, &(an_frames_desc){ .dir = dir, .every = every, .prefix = "space" });
        if (n >= 0) printf("wrote %lld frames to %s\n", (long long)n, dir);
        rc = n >= 0 ? 0 : 1;
    }
    if (mp4 && rc == 0) rc = an_scene_export(s, &(an_export_desc){ .path = mp4 }) > 0 ? 0 : 1;
    if (!dir && !mp4) an_scene_preview(s);
    an_scene_destroy(s);
    return rc;
}
