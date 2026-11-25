// The timeline: recording tracks at the cursor, and sampling them at any time.
// docs/timeline.md is the spec; keep the two in sync.
#include "timeline_internal.h"
#include "ease_internal.h"
#include "text_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

// ---- properties -------------------------------------------------------------------

static const ani_prop_info g_props[AN_PROP_COUNT] = {
    [AN_PROP_POS] = { 2, ANI_SPACE_LINEAR, 0.01 },
    [AN_PROP_SCALE] = { 2, ANI_SPACE_LINEAR, 1e-4 },
    [AN_PROP_ROTATION] = { 1, ANI_SPACE_LINEAR, 1e-4 },
    [AN_PROP_OPACITY] = { 1, ANI_SPACE_LINEAR, 1e-4 },
    [AN_PROP_FILL] = { 4, ANI_SPACE_OKLAB, 1e-4 },
    [AN_PROP_STROKE] = { 4, ANI_SPACE_OKLAB, 1e-4 },
    [AN_PROP_Z] = { 1, ANI_SPACE_STEP, 0.0 },
    [AN_PROP_STROKE_WIDTH] = { 1, ANI_SPACE_LINEAR, 0.01 },
    [AN_PROP_DRAW] = { 1, ANI_SPACE_LINEAR, 1e-4 },
    [AN_PROP_CHAOS] = { 1, ANI_SPACE_LINEAR, 1e-4 },
};

#define SETTLE_RELATIVE 1e-3 // spring tolerance relative to travel distance

const ani_prop_info *ani_prop_info_of(an_prop p) {
    return &g_props[p];
}

int64_t ani_ticks(double seconds) {
    if (!(seconds > 0.0)) return 0; // also NaN
    return llround(seconds * (double)ANI_TICKS_PER_SEC);
}

static ani_value from_color(an_color c) {
    return (ani_value){ { c.r, c.g, c.b, c.a } };
}

static an_color to_color(ani_value v) {
    return (an_color){ (float)v.v[0], (float)v.v[1], (float)v.v[2], (float)v.v[3] };
}

bool ani_prop_valid(ani_target_kind kind, an_prop p) {
    if (p < 0 || p >= AN_PROP_COUNT) return false;
    if (kind == ANI_TARGET_SCENE) return p == AN_PROP_CHAOS; // the scene's only property so far
    return true;
}

ani_value ani_base_get(ani_prop_ref ref) {
    if (ref.kind == ANI_TARGET_SCENE) {
        return ref.prop == AN_PROP_CHAOS ? (ani_value){ { ref.scene->chaos } } : (ani_value){ { 0 } };
    }
    const an_obj *o = ref.obj;
    switch (ref.prop) {
    case AN_PROP_CHAOS: return (ani_value){ { o->chaos } };
    case AN_PROP_POS: return (ani_value){ { o->pos.x, o->pos.y } };
    case AN_PROP_SCALE: return (ani_value){ { o->scale.x, o->scale.y } };
    case AN_PROP_ROTATION: return (ani_value){ { o->rotation } };
    case AN_PROP_OPACITY: return (ani_value){ { o->opacity } };
    case AN_PROP_FILL: return from_color(o->fill);
    case AN_PROP_STROKE: return from_color(o->stroke);
    case AN_PROP_Z: return (ani_value){ { (double)o->z } };
    case AN_PROP_STROKE_WIDTH: return (ani_value){ { o->stroke_width } };
    case AN_PROP_DRAW: return (ani_value){ { o->draw } };
    case AN_PROP_COUNT: break;
    }
    return (ani_value){ { 0 } };
}

static void base_set(ani_prop_ref ref, ani_value v) {
    if (ref.kind == ANI_TARGET_SCENE) {
        if (ref.prop == AN_PROP_CHAOS) ref.scene->chaos = (float)v.v[0];
        return;
    }
    an_obj *o = ref.obj;
    switch (ref.prop) {
    case AN_PROP_CHAOS: o->chaos = (float)v.v[0]; break;
    case AN_PROP_POS: o->pos = (an_vec2){ (float)v.v[0], (float)v.v[1] }; break;
    case AN_PROP_SCALE: o->scale = (an_vec2){ (float)v.v[0], (float)v.v[1] }; break;
    case AN_PROP_ROTATION: o->rotation = (float)v.v[0]; break;
    case AN_PROP_OPACITY: o->opacity = (float)v.v[0]; break;
    case AN_PROP_FILL: o->fill = to_color(v); break;
    case AN_PROP_STROKE: o->stroke = to_color(v); break;
    case AN_PROP_Z: o->z = (int)v.v[0]; break;
    case AN_PROP_STROKE_WIDTH: o->stroke_width = (float)v.v[0]; break;
    case AN_PROP_DRAW: o->draw = (float)v.v[0]; break;
    case AN_PROP_COUNT: break;
    }
}

// Where a target keeps its channels. Adding a target kind (camera) means a new
// case here and in base get/set; tracks and sampling don't change.
static ani_channel **channel_slot(ani_prop_ref ref) {
    switch (ref.kind) {
    case ANI_TARGET_OBJ: return &ref.obj->ch[ref.prop];
    case ANI_TARGET_SCENE: return &ref.scene->ch[ref.prop];
    }
    return NULL;
}

static ani_channel *channel(ani_prop_ref ref) {
    ani_channel **slot = channel_slot(ref);
    return slot ? *slot : NULL;
}

// ---- natural <-> interp space -------------------------------------------------------

static ani_value to_interp(const ani_prop_info *pi, ani_value n) {
    if (pi->space != ANI_SPACE_OKLAB) return n;
    const ani_lab lab = ani_srgb_to_oklab(n.v[0], n.v[1], n.v[2]);
    return (ani_value){ { lab.L, lab.a, lab.b, n.v[3] } };
}

static double clamp01(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }

static ani_value to_natural(an_prop p, const ani_prop_info *pi, ani_value i) {
    if (pi->space == ANI_SPACE_OKLAB) {
        ani_value n;
        ani_oklab_to_srgb((ani_lab){ i.v[0], i.v[1], i.v[2] }, &n.v[0], &n.v[1], &n.v[2]);
        n.v[3] = clamp01(i.v[3]);
        return n;
    }
    if (p == AN_PROP_OPACITY || p == AN_PROP_DRAW || p == AN_PROP_CHAOS) i.v[0] = clamp01(i.v[0]);
    if (p == AN_PROP_STROKE_WIDTH && i.v[0] < 0.0) i.v[0] = 0.0;
    return i;
}

// ---- evaluating one track -----------------------------------------------------------

// Value (interp) and velocity (interp units / s) of track `t` at `elapsed`
// seconds after its start, which is before its end.
static void eval_running(const ani_track *t, int ncomp, double elapsed, ani_value *val, ani_value *vel) {
    *val = (ani_value){ { 0 } };
    *vel = (ani_value){ { 0 } };
    if (t->ease.kind == AN_EASE_KIND_SPRING) {
        const ani_spring sp = ani_spring_params(t->ease);
        for (int c = 0; c < ncomp; c++) {
            double d, dv;
            ani_spring_eval(sp, t->from.v[c] - t->to.v[c], t->v0.v[c], elapsed, &d, &dv);
            val->v[c] = t->to.v[c] + d;
            vel->v[c] = dv;
        }
        return;
    }
    const double dur = (double)t->duration / (double)ANI_TICKS_PER_SEC;
    const double u = elapsed / dur;
    const double e = ani_ease_value(t->ease.kind, u);
    const double de = ani_ease_deriv(t->ease.kind, u);
    for (int c = 0; c < ncomp; c++) {
        const double span = t->to.v[c] - t->from.v[c];
        val->v[c] = t->from.v[c] + span * e;
        vel->v[c] = span * de / dur;
    }
}

typedef enum { AT_BASE, AT_START, AT_END, AT_RUNNING } sample_kind;

// Find the winning track at `at` and evaluate it in interp space.
static sample_kind sample_interp(const ani_channel *ch, int ncomp, ani_time at,
                                 const ani_track **winner, ani_value *val, ani_value *vel) {
    *winner = NULL;
    *vel = (ani_value){ { 0 } };
    if (!ch || ch->count == 0) return AT_BASE;
    // last track with start reached; tracks are sorted by (start, seq)
    int lo = 0, hi = ch->count; // first index whose start is not reached
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (ani_reached(ch->tracks[mid].start, at)) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0) return AT_BASE;
    const ani_track *t = &ch->tracks[lo - 1];
    *winner = t;
    // elapsed, exactly, in units of 1/(den * 1e6) s
    const int64_t elapsed_scaled = at.num * ANI_TICKS_PER_SEC - t->start * at.den;
    if (t->duration == 0 || elapsed_scaled >= t->duration * at.den) {
        *val = t->to;
        return AT_END;
    }
    if (elapsed_scaled == 0) { // exactly at the start: exactly `from`; analytic velocity
        ani_value unused;
        eval_running(t, ncomp, 0.0, &unused, vel);
        *val = t->from;
        return AT_START;
    }
    const double elapsed = (double)elapsed_scaled / ((double)at.den * (double)ANI_TICKS_PER_SEC);
    eval_running(t, ncomp, elapsed, val, vel);
    return AT_RUNNING;
}

ani_value ani_sample_vel(ani_prop_ref ref, ani_time at, ani_value *velocity) {
    const ani_prop_info *pi = ani_prop_info_of(ref.prop);
    const ani_track *t;
    ani_value val, vel;
    const sample_kind k = sample_interp(channel(ref), pi->ncomp, at, &t, &val, &vel);
    if (velocity) *velocity = vel;
    switch (k) {
    case AT_BASE: return ani_base_get(ref);
    case AT_START: return t->from_natural;
    case AT_END: return t->to_natural;
    case AT_RUNNING: break;
    }
    return to_natural(ref.prop, pi, val);
}

ani_value ani_sample(ani_prop_ref ref, ani_time at) {
    return ani_sample_vel(ref, at, NULL);
}

// ---- recording -----------------------------------------------------------------------

static bool insert(an_scene *s, ani_prop_ref ref, const ani_track *t) {
    ani_channel **slot = channel_slot(ref);
    if (!slot) return false;
    ani_channel *ch = *slot;
    if (!ch) {
        ch = ANI_NEW(&s->arena, ani_channel);
        if (!ch) return false;
        *slot = ch;
    }
    if (ch->count == ch->cap) {
        // arena memory can't grow in place; copy into a bigger block
        const int cap = ch->cap ? ch->cap * 2 : 4;
        ani_track *nt = ani_arena_alloc(&s->arena, (size_t)cap * sizeof *nt, _Alignof(ani_track));
        if (!nt) return false;
        if (ch->count) memcpy(nt, ch->tracks, (size_t)ch->count * sizeof *nt);
        ch->tracks = nt;
        ch->cap = cap;
    }
    // keep (start, seq) order; seq only grows, so insert after equal starts
    int i = ch->count;
    while (i > 0 && ch->tracks[i - 1].start > t->start) {
        ch->tracks[i] = ch->tracks[i - 1];
        i--;
    }
    ch->tracks[i] = *t;
    ch->count++;
    return true;
}

int64_t ani_record(an_scene *s, ani_prop_ref ref, int64_t start, int64_t duration,
                   an_ease ease, ani_value value, bool relative, const ani_value *from) {
    const ani_prop_info *pi = ani_prop_info_of(ref.prop);
    ani_track t = { .start = start, .seq = ++s->seq, .ease = ease };

    if (from) {
        // explicit start (e.g. an_draw always starts at 0): a deliberate jump, at rest
        t.from_natural = *from;
        t.from = to_interp(pi, *from);
        t.v0 = (ani_value){ { 0 } };
    } else {
        // from and velocity: the channel as it stands, at the new track's start
        const ani_track *w;
        ani_value vel;
        switch (sample_interp(channel(ref), pi->ncomp, ani_tick_time(start), &w, &t.from, &vel)) {
        case AT_BASE:
            t.from_natural = ani_base_get(ref);
            t.from = to_interp(pi, t.from_natural);
            break;
        case AT_START: t.from_natural = w->from_natural; break;
        case AT_END: t.from_natural = w->to_natural; break;
        case AT_RUNNING: t.from_natural = to_natural(ref.prop, pi, t.from); break;
        }
        // only springs carry momentum; eased tracks start at rest by definition
        t.v0 = (ease.kind == AN_EASE_KIND_SPRING) ? vel : (ani_value){ { 0 } };
    }

    if (relative && pi->space == ANI_SPACE_LINEAR) {
        for (int c = 0; c < pi->ncomp; c++) t.to.v[c] = t.from.v[c] + value.v[c];
        t.to_natural = to_natural(ref.prop, pi, t.to);
    } else {
        t.to = to_interp(pi, value);
        t.to_natural = value;
        if (ref.prop == AN_PROP_OPACITY || ref.prop == AN_PROP_DRAW || ref.prop == AN_PROP_CHAOS)
            t.to_natural.v[0] = clamp01(value.v[0]);
        if (ref.prop == AN_PROP_STROKE_WIDTH && value.v[0] < 0.0) t.to_natural.v[0] = 0.0;
    }

    if (pi->space == ANI_SPACE_STEP) {
        duration = 0;
    } else if (duration < 0) { // spring settle time
        double travel = 0.0;
        for (int c = 0; c < pi->ncomp; c++) {
            const double d = fabs(t.to.v[c] - t.from.v[c]);
            if (d > travel) travel = d;
        }
        double tol = SETTLE_RELATIVE * travel;
        if (tol < pi->floor) tol = pi->floor;
        const ani_spring sp = ani_spring_params(ease);
        double settle = 0.0;
        for (int c = 0; c < pi->ncomp; c++) {
            const double sc = ani_spring_settle(sp, t.from.v[c] - t.to.v[c], t.v0.v[c], tol);
            if (sc > settle) settle = sc;
        }
        duration = (int64_t)ceil(settle * (double)ANI_TICKS_PER_SEC);
    }
    t.duration = duration;

    if (!insert(s, ref, &t)) {
        fprintf(stderr, "anown: out of memory recording a track\n");
        return start;
    }
    const int64_t end = start + duration;
    if (end > s->end) s->end = end;
    return end;
}

void ani_set(ani_prop_ref ref, ani_value natural) {
    an_scene *s = ref.scene;
    const ani_channel *ch = channel(ref);
    const int64_t birth = ref.kind == ANI_TARGET_OBJ ? ref.obj->birth : 0; // the scene exists from 0
    if ((!ch || ch->count == 0) && s->cursor <= birth) {
        base_set(ref, natural); // nothing earlier exists to differ from
        return;
    }
    ani_record(s, ref, s->cursor, 0, AN_EASE_LINEAR, natural, false, NULL);
}

// ---- public: setters -----------------------------------------------------------------

void an_obj_set_pos(an_obj *o, an_vec2 p) {
    if (o) ani_set(ani_obj_prop(o, AN_PROP_POS), (ani_value){ { p.x, p.y } });
}
void an_obj_set_scale(an_obj *o, an_vec2 sc) {
    if (o) ani_set(ani_obj_prop(o, AN_PROP_SCALE), (ani_value){ { sc.x, sc.y } });
}
void an_obj_set_rotation(an_obj *o, float r) {
    if (o) ani_set(ani_obj_prop(o, AN_PROP_ROTATION), (ani_value){ { r } });
}
void an_obj_set_opacity(an_obj *o, float a) {
    if (o) ani_set(ani_obj_prop(o, AN_PROP_OPACITY), (ani_value){ { clamp01(a) } });
}
void an_obj_set_z(an_obj *o, int z) {
    if (o) ani_set(ani_obj_prop(o, AN_PROP_Z), (ani_value){ { (double)z } });
}
void an_obj_set_fill(an_obj *o, an_color c) {
    if (o) ani_set(ani_obj_prop(o, AN_PROP_FILL), from_color(c));
}
void an_obj_set_stroke(an_obj *o, an_color c) {
    if (o) ani_set(ani_obj_prop(o, AN_PROP_STROKE), from_color(c));
}
void an_obj_set_stroke_width(an_obj *o, float w) {
    if (o) ani_set(ani_obj_prop(o, AN_PROP_STROKE_WIDTH), (ani_value){ { w < 0 ? 0 : w } });
}
void an_obj_set_draw(an_obj *o, float p) {
    if (o) ani_set(ani_obj_prop(o, AN_PROP_DRAW), (ani_value){ { clamp01(p) } });
}

// ---- public: anims ---------------------------------------------------------------------

an_ease an_spring(float damping_ratio, float response_seconds) {
    return (an_ease){ .kind = AN_EASE_KIND_SPRING, .damping = damping_ratio, .response = response_seconds };
}

static an_anim anim(an_obj *o, an_prop p, bool rel, float a, float b, float c, float d,
                    const an_anim_opts *opts) {
    return (an_anim){ .obj = o, .prop = p, .relative = rel, .value = { a, b, c, d },
                      .opts = opts ? *opts : (an_anim_opts){ 0 } };
}

an_anim an_move_to(an_obj *o, an_vec2 to, const an_anim_opts *op) { return anim(o, AN_PROP_POS, false, to.x, to.y, 0, 0, op); }
an_anim an_move_by(an_obj *o, an_vec2 d, const an_anim_opts *op) { return anim(o, AN_PROP_POS, true, d.x, d.y, 0, 0, op); }
an_anim an_scale_to(an_obj *o, an_vec2 to, const an_anim_opts *op) { return anim(o, AN_PROP_SCALE, false, to.x, to.y, 0, 0, op); }
an_anim an_rotate_to(an_obj *o, float r, const an_anim_opts *op) { return anim(o, AN_PROP_ROTATION, false, r, 0, 0, 0, op); }
an_anim an_rotate_by(an_obj *o, float r, const an_anim_opts *op) { return anim(o, AN_PROP_ROTATION, true, r, 0, 0, 0, op); }
an_anim an_fade_to(an_obj *o, float a, const an_anim_opts *op) { return anim(o, AN_PROP_OPACITY, false, a, 0, 0, 0, op); }
an_anim an_fill_to(an_obj *o, an_color c, const an_anim_opts *op) { return anim(o, AN_PROP_FILL, false, c.r, c.g, c.b, c.a, op); }
an_anim an_stroke_to(an_obj *o, an_color c, const an_anim_opts *op) { return anim(o, AN_PROP_STROKE, false, c.r, c.g, c.b, c.a, op); }
an_anim an_stroke_width_to(an_obj *o, float w, const an_anim_opts *op) { return anim(o, AN_PROP_STROKE_WIDTH, false, w, 0, 0, 0, op); }
an_anim an_draw_to(an_obj *o, float p, const an_anim_opts *op) { return anim(o, AN_PROP_DRAW, false, p, 0, 0, 0, op); }
an_anim an_draw(an_obj *o, const an_anim_opts *op) {
    an_anim a = anim(o, AN_PROP_DRAW, false, 1, 0, 0, 0, op);
    a.has_from = true; // from[] is zero: always starts at 0
    return a;
}

// Record `a` starting at `start`; returns its end (or start if it can't run).
static int64_t record_anim(an_scene *s, const an_anim *a, int64_t start) {
    if (!s) return start;
    // the target: an object, or the scene itself
    ani_prop_ref ref;
    if (a->obj) {
        if (a->obj->scene != s) {
            fprintf(stderr, "anown: animation on an object from another scene\n");
            return start;
        }
        ref = ani_obj_prop(a->obj, a->prop);
    } else if (a->scene == s) {
        ref = ani_scene_prop(s, a->prop);
    } else {
        return start;
    }
    if (!ani_prop_valid(ref.kind, a->prop)) return start;
    const bool spring = a->opts.ease.kind == AN_EASE_KIND_SPRING;
    int64_t duration = ani_ticks(a->opts.duration);
    if (duration == 0) duration = spring ? -1 : ANI_TICKS_PER_SEC;
    const ani_value v = { { a->value[0], a->value[1], a->value[2], a->value[3] } };
    const ani_value from = { { a->from[0], a->from[1], a->from[2], a->from[3] } };
    return ani_record(s, ref, start, duration, a->opts.ease, v, a->relative,
                      a->has_from ? &from : NULL);
}

void an_play(an_scene *s, an_anim a) {
    if (!s) return;
    const int64_t end = record_anim(s, &a, s->cursor);
    if (end > s->cursor) s->cursor = end;
}

void an_play_async(an_scene *s, an_anim a) {
    if (s) record_anim(s, &a, s->cursor);
}

void an_wait(an_scene *s, float seconds) {
    if (s) s->cursor += ani_ticks(seconds);
}

void an_par(an_scene *s, int n, const an_anim *anims) {
    an_stagger(s, n, anims, 0.0f);
}

void an_stagger(an_scene *s, int n, const an_anim *anims, float gap_seconds) {
    if (!s || !anims) return;
    const int64_t gap = ani_ticks(gap_seconds);
    int64_t latest = s->cursor;
    for (int i = 0; i < n; i++) {
        const int64_t end = record_anim(s, &anims[i], s->cursor + (int64_t)i * gap);
        if (end > latest) latest = end;
    }
    s->cursor = latest;
}

// ---- public: time, seeking -----------------------------------------------------------------

double an_scene_time(const an_scene *s) {
    return (double)s->cursor / (double)ANI_TICKS_PER_SEC;
}

int64_t an_scene_length(const an_scene *s) {
    const int64_t end = s->end > s->cursor ? s->end : s->cursor;
    // ceil(end * fps / 1e6): the first frame at or after the end
    return (end * s->fps + ANI_TICKS_PER_SEC - 1) / ANI_TICKS_PER_SEC;
}

void an_seek(an_scene *s, int64_t frame) {
    if (s) s->frame = frame < 0 ? 0 : frame;
}

an_obj_state an_obj_get(const an_obj *o) {
    if (!o) return (an_obj_state){ 0 };
    an_obj *m = (an_obj *)o; // sampling reads only; the handle type isn't const
    const ani_time at = ani_frame_time(o->scene, o->scene->frame);
    const ani_value pos = ani_sample(ani_obj_prop(m, AN_PROP_POS), at);
    const ani_value sc = ani_sample(ani_obj_prop(m, AN_PROP_SCALE), at);
    return (an_obj_state){
        .exists = ani_obj_exists(o, at),
        .pos = { (float)pos.v[0], (float)pos.v[1] },
        .scale = { (float)sc.v[0], (float)sc.v[1] },
        .rotation = (float)ani_sample(ani_obj_prop(m, AN_PROP_ROTATION), at).v[0],
        .opacity = (float)ani_sample(ani_obj_prop(m, AN_PROP_OPACITY), at).v[0],
        .fill = to_color(ani_sample(ani_obj_prop(m, AN_PROP_FILL), at)),
        .stroke = to_color(ani_sample(ani_obj_prop(m, AN_PROP_STROKE), at)),
        .z = (int)ani_sample(ani_obj_prop(m, AN_PROP_Z), at).v[0],
        .stroke_width = (float)ani_sample(ani_obj_prop(m, AN_PROP_STROKE_WIDTH), at).v[0],
        .draw = (float)ani_sample(ani_obj_prop(m, AN_PROP_DRAW), at).v[0],
    };
}

// ---- chaos (docs/layers.md) ----------------------------------------------------------

void an_scene_set_chaos(an_scene *s, float c) {
    if (s) ani_set(ani_scene_prop(s, AN_PROP_CHAOS), (ani_value){ { clamp01(c) } });
}

void an_obj_set_chaos(an_obj *o, float c) {
    if (o) ani_set(ani_obj_prop(o, AN_PROP_CHAOS), (ani_value){ { clamp01(c) } });
}

an_anim an_chaos_to(an_obj *o, float c, const an_anim_opts *op) {
    return anim(o, AN_PROP_CHAOS, false, c, 0, 0, 0, op);
}

an_anim an_scene_chaos_to(an_scene *s, float c, const an_anim_opts *op) {
    an_anim a = anim(NULL, AN_PROP_CHAOS, false, c, 0, 0, 0, op);
    a.scene = s;
    return a;
}

double ani_scene_chaos_at(an_scene *s, ani_time at) {
    return ani_sample(ani_scene_prop(s, AN_PROP_CHAOS), at).v[0];
}

double ani_obj_chaos_at(an_obj *o, ani_time at) {
    double c = ani_scene_chaos_at(o->scene, at);
    double det = 1.0; // of the world transform: the product of the scales' determinants
    for (an_obj *p = o; p && p->parent; p = p->parent) { // the hidden root isn't a target
        c *= ani_sample(ani_obj_prop(p, AN_PROP_CHAOS), at).v[0];
        const ani_value sc = ani_sample(ani_obj_prop(p, AN_PROP_SCALE), at);
        det *= sc.v[0] * sc.v[1];
    }
    // a letter's size factor (docs/text.md, problem 4), from its scale on screen
    if (o->kind == AN_KIND_GLYPH) {
        const an_style *sty = &o->scene->style;
        c *= ani_text_size_factor(o, (float)sqrt(fabs(det)), sty->boil_off ? 0.0f : sty->boil);
    }
    return c;
}

float an_scene_chaos(const an_scene *s) {
    if (!s) return 0.0f;
    an_scene *m = (an_scene *)s; // sampling only reads
    return (float)ani_scene_chaos_at(m, ani_frame_time(s, s->frame));
}

float an_obj_chaos(const an_obj *o) {
    if (!o) return 0.0f;
    an_obj *m = (an_obj *)o;
    return (float)ani_obj_chaos_at(m, ani_frame_time(o->scene, o->scene->frame));
}
