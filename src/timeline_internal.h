// Internal: tracks, channels, property handles and sampling. docs/timeline.md.
#ifndef ANOWN_TIMELINE_INTERNAL_H
#define ANOWN_TIMELINE_INTERNAL_H

#include "scene_internal.h"

#define ANI_TICKS_PER_SEC 1000000LL

// A point in time, num/den seconds. Frames: {N, fps}. Ticks: {t, 1e6}.
typedef struct ani_time {
    int64_t num, den;
} ani_time;

static inline ani_time ani_frame_time(const an_scene *s, int64_t frame) {
    return (ani_time){ frame, s->fps };
}
static inline ani_time ani_tick_time(int64_t ticks) {
    return (ani_time){ ticks, ANI_TICKS_PER_SEC };
}
// Has the tick `t` been reached at time `at`? Exact integer comparison.
static inline bool ani_reached(int64_t t, ani_time at) {
    return t * at.den <= at.num * ANI_TICKS_PER_SEC;
}

// Seconds <-> ticks. Seconds round to the nearest microsecond.
int64_t ani_ticks(double seconds);

// Values are up to 4 doubles. "Natural" space is what the API uses (sRGB colors);
// "interp" space is where interpolation happens (OKLab + alpha for colors).
typedef struct ani_value {
    double v[4];
} ani_value;

typedef enum ani_space { ANI_SPACE_LINEAR, ANI_SPACE_OKLAB, ANI_SPACE_STEP, ANI_SPACE_LOG } ani_space;

typedef struct ani_prop_info {
    int ncomp;
    ani_space space;
    double floor; // absolute settle tolerance (interp units)
} ani_prop_info;

const ani_prop_info *ani_prop_info_of(an_prop p);

// Generic property handle: what a track animates. Only object targets have
// properties today; scene and camera targets slot in here later.
typedef enum ani_target_kind { ANI_TARGET_OBJ, ANI_TARGET_SCENE } ani_target_kind;

typedef struct ani_prop_ref {
    ani_target_kind kind;
    an_obj *obj;     // ANI_TARGET_OBJ
    an_scene *scene; // always set: the scene that owns the target
    an_prop prop;
} ani_prop_ref;

static inline ani_prop_ref ani_obj_prop(an_obj *o, an_prop p) {
    return (ani_prop_ref){ .kind = ANI_TARGET_OBJ, .obj = o, .scene = o->scene, .prop = p };
}

static inline ani_prop_ref ani_scene_prop(an_scene *s, an_prop p) {
    return (ani_prop_ref){ .kind = ANI_TARGET_SCENE, .scene = s, .prop = p };
}

// Which properties each target kind has.
bool ani_prop_valid(ani_target_kind kind, an_prop p);

// Chaos at a time: the scene dial, and an object's effective chaos (scene dial x
// the object multipliers from the top level down). Pure.
double ani_scene_chaos_at(an_scene *s, ani_time at);

// The camera at a time (docs/camera.md): position, zoom, rotation.
typedef struct ani_camera {
    an_vec2 pos;
    float zoom, rot;
    an_vec2 shake; // screen px, applied after zoom and rotation
} ani_camera;
ani_camera ani_camera_at(an_scene *s, ani_time at);
// The camera shake offset alone (screen px) at a time: a pure function of the
// scene's seed, its shake events and the time (docs/camera.md).
an_vec2 ani_shake_at(const an_scene *s, ani_time at);
// An object's area scale on screen at a time: camera zoom x sqrt |det| of its
// and its ancestors' scales (verb scales included once verbs exist).
double ani_obj_area_scale_at(an_obj *o, ani_time at);
// Record an an_anim at `start` (what an_play does, without moving the cursor).
int64_t ani_record_anim(an_scene *s, const an_anim *a, int64_t start);
// array.c: the tracks of an_array_swap (docs/explainer.md).
int64_t ani_array_record_swap(an_scene *s, const an_anim *a, int64_t start);
// An object's world transform at a time (without the camera), from the top down.
struct ani_affine ani_obj_world_at(an_obj *o, ani_time at);
// Seconds from a spring's start (at rest) until it first reaches its target;
// -1 for a critically damped spring (docs/camera.md, the slap's impact).
double ani_spring_first_arrival(an_ease e);
double ani_obj_chaos_at(an_obj *o, ani_time at);

typedef struct ani_track {
    int64_t start, duration; // ticks; duration 0 = step
    uint32_t seq;
    an_ease ease;
    ani_value from, to, v0;  // interp space; v0 in interp units per second
    ani_value from_natural;  // exact natural start value
    ani_value to_natural;    // exact natural end value
    // an_move_arc (docs/explainer.md): a cubic Bezier from -> c1 -> c2 -> to at the
    // eased progress, instead of a straight line (2 components, linear space)
    bool arc;
    ani_value c1, c2;
    // a lifted arc (array swaps): rise along the normal until the eased progress
    // reaches lift (no travel), travel as a smoothstep over [lift, 1 - lift], land.
    // Height profile 4 H u (1 - u) along the normal, H = arc height. 0: plain Bezier.
    double lift, height;
    double nx, ny;
} ani_track;

struct ani_channel {
    ani_track *tracks; // sorted by (start, seq)
    int count, cap;
};

// Base (natural) value of a property.
ani_value ani_base_get(ani_prop_ref ref);
// Sampled natural value at time `at` (base + winning track).
ani_value ani_sample(ani_prop_ref ref, ani_time at);
// Same, also returning the analytic velocity (interp units per second; 0 if
// no track is running). Pure: used for the nozzle speed.
ani_value ani_sample_vel(ani_prop_ref ref, ani_time at, ani_value *velocity);

// Record a track at `start`. duration < 0: spring settle time (springs only).
// If `relative`, value is a delta added to the value at `start` (linear props).
// If `from` is given, the track starts there (natural space) at rest, instead of
// continuing from the channel.
// Returns the track's end in ticks.
// The same, along an arc of `height` (docs/explainer.md, an_move_arc): pos-like
// 2-component linear properties only. A spring ease is treated as in-out.
int64_t ani_record_arc(an_scene *s, ani_prop_ref ref, int64_t start, int64_t duration, an_ease ease,
                       ani_value to, double height);
// A lifted arc (docs/explainer.md, swaps): no travel while the eased progress is
// below `lift` (or above 1 - lift), height profile 4 H u (1 - u).
int64_t ani_record_lift(an_scene *s, ani_prop_ref ref, int64_t start, int64_t duration, an_ease ease,
                        ani_value to, double height, double lift);
int64_t ani_record(an_scene *s, ani_prop_ref ref, int64_t start, int64_t duration,
                   an_ease ease, ani_value value, bool relative, const ani_value *from);

// Setter semantics: zero-length take-over track at the cursor, or a base write
// when nothing could tell the difference.
void ani_set(ani_prop_ref ref, ani_value natural);

// Does the object exist at `at`?
static inline bool ani_obj_exists(const an_obj *o, ani_time at) {
    return ani_reached(o->birth, at);
}

#endif
