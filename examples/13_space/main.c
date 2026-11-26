// 13_space: the 3D space (docs/space.md). Milestone A: a grid seen straight
// from above (it looks like the 2D plane) tilts back into 3D, the camera orbits
// around three vectors, one of them leaving the floor, and the space takes a 3x3
// matrix, then the camera dollies in. Milestone B: a spinning globe whose axis is
// its eigenvector (lambda = 1, drawn over the globe), and an egg: a sphere
// stretched by diag(3, 2, 1) and turned, its three axis arrows stretching with it.
//
//   ./build/examples/13_space [--frames DIR] [--every N] [--export FILE.mp4]
#include "anown.h"

#include <math.h>
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
    // ---- milestone B: the globe and the egg ----------------------------------------------------
    an_play_async(s, an_space_matrix_to(sp, AN_MAT3_IDENTITY, &(an_anim_opts){ .duration = 1.2f }));
    an_obj *gone[5] = { u, v, w, tip, drop };
    for (int k = 0; k < 5; k++) an_play_async(s, an_fade_to(gone[k], 0, &(an_anim_opts){ .duration = 0.6f }));
    an_play_async(s, an_space_target_to(sp, (an_vec3){ 0, 0, 1.3f }, &(an_anim_opts){ .duration = 1.8f }));
    an_play(s, an_space_orbit_to(sp, (an_orbit){ -0.55f, 1.12f, 15 }, &(an_anim_opts){ .duration = 1.8f }));

    // the globe: a filled sphere in a frame tilted 23.4 degrees, spinning about its axis
    const float tilt = 0.4084f;
    an_obj *globe = an_space_frame(sp, &(an_frame_opts){ .origin = { -3.2f, 0, 1.7f }, .spin_axis = { sinf(tilt), 0, cosf(tilt) } });
    an_obj *ball = an_space_sphere(globe, &(an_sphere_opts){ .radius = 1.5f, .fill = AN_HEX(0xF3EDE2), .back = 0.22f,
                                                             .stroke_width = 3, .hidden = true, .riso = AN_OFF, .shadow = AN_OFF });
    // the axis, in a non-spinning frame with the same origin, along the spin axis: it
    // draws over the globe (see-through), its upper half an arrow, lit orange
    an_obj *ax = an_space_frame(sp, &(an_frame_opts){ .origin = { -3.2f, 0, 1.7f } });
    const an_vec3 up = { 2.5f * sinf(tilt), 0, 2.5f * cosf(tilt) };
    an_obj *axis_lo = an_space_path(ax, (an_vec3[]){ { 0, 0, 0 }, { -up.x, -up.y, -up.z } }, 2, false,
                                    &(an_line_opts){ .stroke = ORANGE, .stroke_width = 5, .undrawn = true, .riso = AN_OFF, .shadow = AN_OFF });
    an_obj *axis_hi = an_space_vector(ax, up, &(an_arrow_opts){ .stroke = ORANGE, .stroke_width = 7, .undrawn = true,
                                                                 .riso = AN_OFF, .shadow = AN_OFF });
    an_obj *lam = an_text(s, &(an_text_opts){ .parent = ax, .text = "\xCE\xBB = 1", .size = 46, .font = an_font_mono(s),
                                               .align = AN_ALIGN_LEFT, .anchor = AN_ANCHOR_MIDDLE, .fill = ORANGE,
                                               .hidden = true, .riso = AN_OFF, .shadow = AN_OFF });
    an_obj_set_pos3(lam, (an_vec3){ up.x * 1.08f + 0.2f, 0, up.z * 1.08f });
    an_play(s, an_fade_to(ball, 1, &(an_anim_opts){ .duration = 0.8f }));
    an_play_async(s, an_frame_spin_to(globe, 6 * PI, &(an_anim_opts){ .duration = 14, .ease = AN_EASE_LINEAR }));
    an_wait(s, 1.5f);
    an_play_async(s, an_draw(axis_lo, &(an_anim_opts){ .duration = 0.5f }));
    an_play(s, an_draw(axis_hi, &(an_anim_opts){ .duration = 0.7f }));
    an_play(s, an_fade_to(lam, 1, &(an_anim_opts){ .duration = 0.4f }));
    an_wait(s, 1.5f);

    // the egg: a sphere of radius 0.8 with arrows along its axes; then F = R diag(3, 2, 1)
    an_obj *egg = an_space_frame(sp, &(an_frame_opts){ .origin = { 3.0f, 0.3f, 1.5f } });
    an_obj *shell = an_space_sphere(egg, &(an_sphere_opts){ .radius = 0.8f, .meridians = 10, .parallels = 5, .back = 0.22f,
                                                            .hidden = true, .riso = AN_OFF, .shadow = AN_OFF });
    static const an_color cols[3] = { MAGENTA, CYAN, ORANGE };
    static const char *const stretch[3] = { "\xC3\x97" "3", "\xC3\x97" "2", "\xC3\x97" "1" };
    an_obj *arms[3], *tags[3];
    for (int k = 0; k < 3; k++) {
        const an_vec3 e = { k == 0 ? 0.8f : 0, k == 1 ? 0.8f : 0, k == 2 ? 0.8f : 0 };
        arms[k] = an_space_vector(egg, e, &(an_arrow_opts){ .stroke = cols[k], .stroke_width = 7, .undrawn = true,
                                                            .riso = AN_OFF, .shadow = AN_OFF });
        tags[k] = an_text(s, &(an_text_opts){ .parent = egg, .text = stretch[k], .size = 42, .font = an_font_mono(s),
                                               .align = AN_ALIGN_CENTER, .anchor = AN_ANCHOR_MIDDLE, .fill = cols[k],
                                               .hidden = true, .riso = AN_OFF, .shadow = AN_OFF });
        an_obj_set_pos3(tags[k], (an_vec3){ e.x * 1.35f, e.y * 1.35f, e.z * 1.35f + (k == 2 ? 0.1f : 0) });
    }
    an_play(s, an_fade_to(shell, 1, &(an_anim_opts){ .duration = 0.6f }));
    for (int k = 0; k < 3; k++) an_play(s, an_draw(arms[k], &(an_anim_opts){ .duration = 0.45f }));
    an_wait(s, 0.4f);
    const an_mat3 Rz = an_mat3_rotation((an_vec3){ 0, 0, 1 }, 0.6f), Rx = an_mat3_rotation((an_vec3){ 1, 0, 0 }, 0.35f);
    an_mat3 Rm, F;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            Rm.m[3 * i + j] = Rz.m[3 * i] * Rx.m[j] + Rz.m[3 * i + 1] * Rx.m[3 + j] + Rz.m[3 * i + 2] * Rx.m[6 + j];
        }
    static const float d[3] = { 3, 2, 1 };
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) F.m[3 * i + j] = Rm.m[3 * i + j] * d[j]; // R diag(3, 2, 1)
    an_play_async(s, an_space_orbit_to(sp, (an_orbit){ 0.5f, 1.05f, 15 }, &(an_anim_opts){ .duration = 8, .ease = AN_EASE_LINEAR }));
    an_play(s, an_space_matrix_to(egg, F, &(an_anim_opts){ .duration = 2.5f }));
    for (int k = 0; k < 3; k++) an_play_async(s, an_fade_to(tags[k], 1, &(an_anim_opts){ .duration = 0.4f }));
    an_wait(s, 6.0f);
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
