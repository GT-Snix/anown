// Easing, springs and OKLab: golden values, derivatives, settle, gamut clamp.
// Golden constants were computed independently with `bc -l` from the closed
// forms in docs/timeline.md (not with this code).
#include "anown.h"
#include "check.h"
#include "ease_internal.h"
#include "timeline_internal.h"

#define PI_D 3.14159265358979323846

// ---- easing ----------------------------------------------------------------------

static void test_easing_golden(void) {
    CHECK_NEAR(ani_ease_value(AN_EASE_KIND_IN_OUT, 0.25), 0.0625, 1e-15);
    CHECK_NEAR(ani_ease_value(AN_EASE_KIND_IN_OUT, 0.5), 0.5, 1e-15);
    CHECK_NEAR(ani_ease_value(AN_EASE_KIND_IN_OUT, 0.75), 0.9375, 1e-15);
    CHECK(ani_ease_value(AN_EASE_KIND_IN_OUT, 0.0) == 0.0);
    CHECK(ani_ease_value(AN_EASE_KIND_IN_OUT, 1.0) == 1.0);
    CHECK_NEAR(ani_ease_value(AN_EASE_KIND_LINEAR, 0.3), 0.3, 1e-15);
    // derivatives against central differences
    const an_ease_kind kinds[2] = { AN_EASE_KIND_LINEAR, AN_EASE_KIND_IN_OUT };
    for (int k = 0; k < 2; k++) {
        for (double u = 0.05; u < 1.0; u += 0.1) {
            const double h = 1e-6;
            const double fd = (ani_ease_value(kinds[k], u + h) - ani_ease_value(kinds[k], u - h)) / (2 * h);
            CHECK_NEAR(ani_ease_deriv(kinds[k], u), fd, 1e-6);
        }
    }
}

// ---- springs: closed form -----------------------------------------------------------

typedef struct golden {
    float zeta;
    double t, e;
} golden;

// response 2*pi s -> omega = 1 rad/s; e(t) = progress from 0 to 1 at rest
static const golden SPRING_GOLDEN[] = {
    { 1.0f, 0.5, 0.09020401043104986460 },
    { 1.0f, 1.0, 0.26424111765711535682 },
    { 1.0f, 2.0, 0.59399415029016192433 },
    { 1.0f, 3.0, 0.80085172652854422812 },
    { 0.5f, 0.5, 0.10440547345507939551 },
    { 0.5f, 1.0, 0.34029984660829833804 },
    { 0.5f, 2.0, 0.84942563485411238623 },
    { 0.5f, 3.0, 1.12435476740841176655 },
    { 0.2f, 0.5, 0.11468118397747091921 },
    { 0.2f, 1.0, 0.40503376736211222501 },
    { 0.2f, 2.0, 1.12748444506208396433 },
    { 0.2f, 3.0, 1.51513211678419431833 },
};

static void test_spring_closed_form(void) {
    for (size_t i = 0; i < sizeof SPRING_GOLDEN / sizeof SPRING_GOLDEN[0]; i++) {
        const golden g = SPRING_GOLDEN[i];
        const ani_spring sp = ani_spring_params(an_spring(g.zeta, (float)(2.0 * PI_D)));
        double d, dv;
        ani_spring_eval(sp, -1.0, 0.0, g.t, &d, &dv); // from 0 to 1
        CHECK_NEAR(1.0 + d, g.e, 1e-6);               // float response -> ~1e-7
        // velocity matches a central difference
        double dp, dm, unused;
        ani_spring_eval(sp, -1.0, 0.0, g.t + 1e-6, &dp, &unused);
        ani_spring_eval(sp, -1.0, 0.0, g.t - 1e-6, &dm, &unused);
        CHECK_NEAR(dv, (dp - dm) / 2e-6, 1e-6);
    }
    // zeta is clamped to [0.01, 1]
    CHECK(ani_spring_params(an_spring(5.0f, 1.0f)).zeta == 1.0);
    CHECK(ani_spring_params(an_spring(0.0f, 1.0f)).zeta == 0.01);
    CHECK_NEAR(ani_spring_params(an_spring(1.0f, 0.0f)).omega, 2 * PI_D / 0.5, 1e-12);
}

// The same spring through the timeline, sampled at known frames, at 30 and 60 fps:
// same seconds -> same value (frame-rate independent).
static void test_spring_frames(void) {
    const int fps_list[2] = { 30, 60 };
    for (int f = 0; f < 2; f++) {
        const int fps = fps_list[f];
        for (size_t i = 0; i < sizeof SPRING_GOLDEN / sizeof SPRING_GOLDEN[0]; i++) {
            const golden g = SPRING_GOLDEN[i];
            an_scene *s = an_scene_create(&(an_scene_desc){ .fps = fps });
            an_obj *o = an_rect(s, NULL); // pos.x 0 -> 1000
            an_play(s, an_move_to(o, (an_vec2){ 1000, 0 },
                                  &(an_anim_opts){ .ease = an_spring(g.zeta, (float)(2.0 * PI_D)) }));
            const int64_t frame = (int64_t)(g.t * fps + 0.5);
            an_seek(s, frame);
            CHECK_NEAR(an_obj_get(o).pos.x, 1000.0 * g.e, 2e-3); // float storage of pos
            an_scene_destroy(s);
        }
    }
    // a faster spring (response 0.5 s, zeta 0.5): bc values at t = 0.1, 0.25, 0.5
    const double t[3] = { 0.1, 0.25, 0.5 };
    const double e[3] = { 0.47961214688455489696, 1.14069967414351761096, 0.98982212901473562286 };
    for (int i = 0; i < 3; i++) {
        an_scene *s = an_scene_create(&(an_scene_desc){ .fps = 60 });
        an_obj *o = an_rect(s, NULL);
        an_play(s, an_move_to(o, (an_vec2){ 0, 100 }, &(an_anim_opts){ .ease = an_spring(0.5f, 0.5f) }));
        an_seek(s, (int64_t)(t[i] * 60 + 0.5));
        CHECK_NEAR(an_obj_get(o).pos.y, 100.0 * e[i], 1e-3);
        an_scene_destroy(s);
    }
}

// Settle: the envelope bound is under tol at the settle time, the last frame is
// exactly the target, and the tolerance scales with travel distance.
static void test_spring_settle(void) {
    const float zetas[3] = { 1.0f, 0.5f, 0.2f };
    for (int i = 0; i < 3; i++) {
        const ani_spring sp = ani_spring_params(an_spring(zetas[i], 0.6f));
        const double tol = 1e-3;
        const double ts = ani_spring_settle(sp, -1.0, 0.0, tol);
        CHECK(ts > 0.0);
        // from ts on, the actual error stays under tol
        for (double t = ts; t < ts + 5.0; t += 0.01) {
            double d, dv;
            ani_spring_eval(sp, -1.0, 0.0, t, &d, &dv);
            CHECK(fabs(d) <= tol + 1e-12);
        }
        // and the bound isn't trivially loose: shortly before, the envelope is above tol
        // (underdamped: check the envelope itself, the oscillation may cross zero)
        if (zetas[i] < 1.0f) {
            const double A = 1.0 / sqrt(1.0 - (double)zetas[i] * zetas[i]); // sqrt(d0^2 + B^2) for d0=-1, v0=0
            CHECK(A * exp(-sp.zeta * sp.omega * (ts - 0.01)) > tol);
        }
    }

    // through the timeline: final frame exactly at target; relative tolerance
    an_scene *s = an_scene_create(&(an_scene_desc){ .fps = 60 });
    an_obj *near = an_rect(s, NULL);
    an_obj *far = an_rect(s, NULL);
    an_play_async(s, an_move_to(near, (an_vec2){ 10, 0 }, &(an_anim_opts){ .ease = an_spring(0.5f, 0.6f) }));
    an_play_async(s, an_move_to(far, (an_vec2){ 10000, 0 }, &(an_anim_opts){ .ease = an_spring(0.5f, 0.6f) }));
    const ani_channel *cn = near->ch[AN_PROP_POS];
    const ani_channel *cf = far->ch[AN_PROP_POS];
    // tol = max(1e-3 * travel, 0.01 px): 0.01 for both 10 px and... 10 px * 1e-3 = 0.01;
    // for 10000 px it's 10 px, so the far spring settles in the same time as the
    // near one would to 0.01 px: identical relative behaviour -> identical durations.
    CHECK(cn->tracks[0].duration == cf->tracks[0].duration);
    const int64_t L = an_scene_length(s);
    an_seek(s, L);
    CHECK(an_obj_get(near).pos.x == 10.0f && an_obj_get(far).pos.x == 10000.0f);
    an_scene_destroy(s);

    // absolute floor: a 1 px move uses tol = 0.01 px (not 1e-3 px), so it settles
    // sooner than a 10 px move with relative tolerance 0.01 px would scale to
    an_scene *s2 = an_scene_create(NULL);
    an_obj *tiny = an_rect(s2, NULL);
    an_obj *ten = an_rect(s2, NULL);
    an_play_async(s2, an_move_to(tiny, (an_vec2){ 1, 0 }, &(an_anim_opts){ .ease = an_spring(1.0f, 0.6f) }));
    an_play_async(s2, an_move_to(ten, (an_vec2){ 10, 0 }, &(an_anim_opts){ .ease = an_spring(1.0f, 0.6f) }));
    CHECK(tiny->ch[AN_PROP_POS]->tracks[0].duration < ten->ch[AN_PROP_POS]->tracks[0].duration);
    an_scene_destroy(s2);
}

// ---- OKLab ---------------------------------------------------------------------------

static void test_oklab_golden(void) {
    // Ottosson's published values for the sRGB primaries (also matched with bc)
    ani_lab r = ani_srgb_to_oklab(1, 0, 0);
    CHECK_NEAR(r.L, 0.62795536, 1e-7);
    CHECK_NEAR(r.a, 0.22486306, 1e-7);
    CHECK_NEAR(r.b, 0.12584630, 1e-7);
    ani_lab g = ani_srgb_to_oklab(0, 1, 0);
    CHECK_NEAR(g.L, 0.86643961, 1e-7);
    CHECK_NEAR(g.a, -0.23388757, 1e-7);
    CHECK_NEAR(g.b, 0.17949848, 1e-7);
    ani_lab b = ani_srgb_to_oklab(0, 0, 1);
    CHECK_NEAR(b.L, 0.45201372, 1e-7);
    CHECK_NEAR(b.a, -0.03245698, 1e-7);
    CHECK_NEAR(b.b, -0.31152815, 1e-7);
    ani_lab w = ani_srgb_to_oklab(1, 1, 1);
    CHECK_NEAR(w.L, 1.0, 1e-7);
    CHECK_NEAR(w.a, 0.0, 1e-7);
    CHECK_NEAR(w.b, 0.0, 1e-7);

    // round trip
    for (double x = 0.0; x <= 1.0; x += 0.125) {
        double rr, gg, bb;
        ani_oklab_to_srgb(ani_srgb_to_oklab(x, 1.0 - x, 0.5), &rr, &gg, &bb);
        CHECK_NEAR(rr, x, 1e-6);
        CHECK_NEAR(gg, 1.0 - x, 1e-6);
        CHECK_NEAR(bb, 0.5, 1e-6);
    }
}

// Pink FF48B0 -> green 00A95C, linear, halfway: OKLab midpoint (bc golden),
// not the muddy sRGB average (0.5, 0.473, 0.525).
static void test_color_track_midpoint(void) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .fps = 60 });
    an_obj *o = an_rect(s, &(an_rect_opts){ .fill = AN_HEX(0xFF48B0) });
    an_play(s, an_fill_to(o, AN_HEX(0x00A95C), &(an_anim_opts){ .duration = 1, .ease = AN_EASE_LINEAR }));
    an_seek(s, 30);
    const an_color c = an_obj_get(o).fill;
    CHECK_NEAR(c.r, 0.68934848690287331, 1e-6);
    CHECK_NEAR(c.g, 0.54180049271935909, 1e-6);
    CHECK_NEAR(c.b, 0.53657976360828317, 1e-6);
    CHECK(c.a == 1.0f);
    // endpoints are the exact sRGB values, not round-tripped
    an_seek(s, 0);
    CHECK(an_obj_get(o).fill.r == AN_HEX(0xFF48B0).r && an_obj_get(o).fill.b == AN_HEX(0xFF48B0).b);
    an_seek(s, 60);
    CHECK(an_obj_get(o).fill.g == AN_HEX(0x00A95C).g && an_obj_get(o).fill.b == AN_HEX(0x00A95C).b);
    an_scene_destroy(s);
}

static int linear_in_gamut(ani_lab c, double eps) {
    double r, g, b;
    ani_oklab_to_srgb_raw(c, &r, &g, &b);
    return r >= -eps && r <= 1 + eps && g >= -eps && g <= 1 + eps && b >= -eps && b <= 1 + eps;
}

static void test_gamut_clamp(void) {
    // far outside sRGB: very saturated orange-red at mid lightness
    const ani_lab out = { 0.7, 0.3, 0.1 };
    CHECK(!linear_in_gamut(out, 0.0));
    double r, g, b;
    ani_oklab_to_srgb(out, &r, &g, &b);
    CHECK(r >= 0 && r <= 1 && g >= 0 && g <= 1 && b >= 0 && b <= 1);
    const ani_lab got = ani_srgb_to_oklab(r, g, b);
    CHECK_NEAR(got.L, 0.7, 1e-6);                               // lightness kept
    CHECK_NEAR(atan2(got.b, got.a), atan2(0.1, 0.3), 1e-5);      // hue kept
    // chroma is (nearly) maximal: 1% more is out of gamut
    CHECK(!linear_in_gamut((ani_lab){ got.L, got.a * 1.01, got.b * 1.01 }, 0.0));
    // lightness overshoot (springs) clips instead of producing garbage
    ani_oklab_to_srgb((ani_lab){ 1.2, 0.0, 0.0 }, &r, &g, &b);
    CHECK(r == 1.0 && g == 1.0 && b == 1.0);
    ani_oklab_to_srgb((ani_lab){ -0.1, 0.05, 0.0 }, &r, &g, &b);
    CHECK(r == 0.0 && g == 0.0 && b == 0.0);
}

int main(void) {
    test_easing_golden();
    test_spring_closed_form();
    test_spring_frames();
    test_spring_settle();
    test_oklab_golden();
    test_color_track_midpoint();
    test_gamut_clamp();
    return check_done("test_ease");
}
