// an_obj_nozzle: the pen tip of a stroke being drawn on. A pure function of
// (path, frame): nothing here writes to the object or the scene.
#include "stroke_internal.h"
#include "timeline_internal.h"

#include <math.h>

// World transform at `at`: sampled local transforms from the top level down.
static ani_affine world_at(an_obj *o, ani_time at) {
    if (!o->parent) return (ani_affine){ 1, 0, 0, 1, 0, 0 };
    const ani_value pos = ani_sample(ani_obj_prop(o, AN_PROP_POS), at);
    const ani_value sc = ani_sample(ani_obj_prop(o, AN_PROP_SCALE), at);
    const ani_value rot = ani_sample(ani_obj_prop(o, AN_PROP_ROTATION), at);
    const ani_affine local = ani_local((an_vec2){ (float)pos.v[0], (float)pos.v[1] },
                                       (an_vec2){ (float)sc.v[0], (float)sc.v[1] }, (float)rot.v[0]);
    return ani_affine_mul(world_at(o->parent, at), local);
}

an_nozzle an_obj_nozzle(const an_obj *o) {
    if (!o || !o->path || o->path->total <= 0.0f) return (an_nozzle){ 0 };
    an_obj *m = (an_obj *)o; // sampling only reads; handles are non-const
    const ani_time at = ani_frame_time(o->scene, o->scene->frame);

    ani_value dp; // d(progress)/dt, analytic, from the draw track's easing
    const double p = ani_sample_vel(ani_obj_prop(m, AN_PROP_DRAW), at, &dp).v[0];
    const ani_affine w = world_at(m, at);

    const double s = p * o->path->total;
    an_vec2 lp, lt;
    int sub;
    double s_in;
    ani_path_at(o->path, s, &lp, &lt, &sub, &s_in);

    // tangent through the linear part of the world transform
    const an_vec2 wt = { w.a * lt.x + w.c * lt.y, w.b * lt.x + w.d * lt.y };
    const double wt_len = hypot(wt.x, wt.y);
    const float wscale = sqrtf(fabsf(w.a * w.d - w.b * w.c));
    const double width = ani_sample(ani_obj_prop(m, AN_PROP_STROKE_WIDTH), at).v[0];
    const double len_sub = o->path->subs[sub].len;

    // progress clamped at 0 or 1 (e.g. a spring overshooting) means the pen is still
    const bool clamped = p <= 0.0 || p >= 1.0;
    return (an_nozzle){
        .active = ani_obj_exists(o, at) && p > 0.0 && p < 1.0,
        .pos = ani_affine_apply(w, lp),
        .dir = wt_len > 0 ? (an_vec2){ (float)(wt.x / wt_len), (float)(wt.y / wt_len) } : (an_vec2){ 1, 0 },
        .speed = clamped ? 0.0f : (float)(fabs(dp.v[0]) * o->path->total * wt_len),
        .width = (float)(width * wscale * ani_brush_factor(&o->brush, s_in, len_sub)),
    };
}
