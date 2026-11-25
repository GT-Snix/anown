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
    [AN_PROP_CAMERA_POS] = { 2, ANI_SPACE_LINEAR, 0.01 },
    [AN_PROP_CAMERA_ZOOM] = { 1, ANI_SPACE_LOG, 1e-4 },
    [AN_PROP_CAMERA_ROTATION] = { 1, ANI_SPACE_LINEAR, 1e-4 },
    [AN_PROP_CAMERA_SHAKE] = { 1, ANI_SPACE_STEP, 0.0 }, // events, see an_shake
    [AN_PROP_VERB_SCALE] = { 2, ANI_SPACE_LINEAR, 1e-4 },
    [AN_PROP_VERB_ROTATION] = { 1, ANI_SPACE_LINEAR, 1e-4 },
    [AN_PROP_VERB_PIVOT] = { 2, ANI_SPACE_STEP, 0.0 },
    [AN_PROP_CELL_STATE] = { 1, ANI_SPACE_STEP, 0.0 },
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
    const bool camera = p == AN_PROP_CAMERA_POS || p == AN_PROP_CAMERA_ZOOM || p == AN_PROP_CAMERA_ROTATION ||
                        p == AN_PROP_CAMERA_SHAKE;
    if (kind == ANI_TARGET_SCENE) return p == AN_PROP_CHAOS || camera;
    return !camera;
}

ani_value ani_base_get(ani_prop_ref ref) {
    if (ref.kind == ANI_TARGET_SCENE) {
        const an_scene *s = ref.scene;
        switch (ref.prop) {
        case AN_PROP_CHAOS: return (ani_value){ { s->chaos } };
        case AN_PROP_CAMERA_POS: return (ani_value){ { s->cam_pos.x, s->cam_pos.y } };
        case AN_PROP_CAMERA_ZOOM: return (ani_value){ { s->cam_zoom } };
        case AN_PROP_CAMERA_ROTATION: return (ani_value){ { s->cam_rot } };
        default: return (ani_value){ { 0 } };
        }
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
    case AN_PROP_VERB_SCALE: return (ani_value){ { o->vscale.x, o->vscale.y } };
    case AN_PROP_VERB_ROTATION: return (ani_value){ { o->vrot } };
    case AN_PROP_VERB_PIVOT: return (ani_value){ { o->vpivot.x, o->vpivot.y } };
    case AN_PROP_CELL_STATE: return (ani_value){ { (double)o->cell_state } };
    default: break; // camera properties aren't the object's
    }
    return (ani_value){ { 0 } };
}

static void base_set(ani_prop_ref ref, ani_value v) {
    if (ref.kind == ANI_TARGET_SCENE) {
        an_scene *s = ref.scene;
        if (ref.prop == AN_PROP_CHAOS) s->chaos = (float)v.v[0];
        if (ref.prop == AN_PROP_CAMERA_POS) s->cam_pos = (an_vec2){ (float)v.v[0], (float)v.v[1] };
        if (ref.prop == AN_PROP_CAMERA_ZOOM) s->cam_zoom = (float)v.v[0];
        if (ref.prop == AN_PROP_CAMERA_ROTATION) s->cam_rot = (float)v.v[0];
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
    case AN_PROP_VERB_SCALE: o->vscale = (an_vec2){ (float)v.v[0], (float)v.v[1] }; break;
    case AN_PROP_VERB_ROTATION: o->vrot = (float)v.v[0]; break;
    case AN_PROP_VERB_PIVOT: o->vpivot = (an_vec2){ (float)v.v[0], (float)v.v[1] }; break;
    case AN_PROP_CELL_STATE: o->cell_state = (int)v.v[0]; break;
    default: break;
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
    if (pi->space == ANI_SPACE_LOG) return (ani_value){ { log(n.v[0] > 1e-9 ? n.v[0] : 1e-9) } };
    if (pi->space != ANI_SPACE_OKLAB) return n;
    const ani_lab lab = ani_srgb_to_oklab(n.v[0], n.v[1], n.v[2]);
    return (ani_value){ { lab.L, lab.a, lab.b, n.v[3] } };
}

static double clamp01(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }

static ani_value to_natural(an_prop p, const ani_prop_info *pi, ani_value i) {
    if (pi->space == ANI_SPACE_LOG) return (ani_value){ { exp(i.v[0]) } };
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
    if (t->arc && t->lift > 0.0) { // lift: travel only over [lift, 1 - lift]
        const double L = t->lift, span = 1.0 - 2.0 * L;
        double k = (e - L) / span, dk = 1.0 / span;
        if (k <= 0.0) { k = 0.0; dk = 0.0; }
        if (k >= 1.0) { k = 1.0; dk = 0.0; }
        const double S = k * k * (3.0 - 2.0 * k), dS = 6.0 * k * (1.0 - k) * dk;
        const double hgt = 4.0 * t->height * e * (1.0 - e), dh = 4.0 * t->height * (1.0 - 2.0 * e);
        const double nn[2] = { t->nx, t->ny };
        for (int c = 0; c < 2; c++) {
            const double span_c = t->to.v[c] - t->from.v[c];
            val->v[c] = t->from.v[c] + span_c * S + nn[c] * hgt;
            vel->v[c] = (span_c * dS + nn[c] * dh) * de / dur;
        }
        return;
    }
    if (t->arc) { // B(e) and B'(e) e' / dur
        const double a = 1.0 - e;
        for (int c = 0; c < 2; c++) {
            const double p0 = t->from.v[c], p1 = t->c1.v[c], p2 = t->c2.v[c], p3 = t->to.v[c];
            val->v[c] = a * a * a * p0 + 3.0 * a * a * e * p1 + 3.0 * a * e * e * p2 + e * e * e * p3;
            const double db = 3.0 * a * a * (p1 - p0) + 6.0 * a * e * (p2 - p1) + 3.0 * e * e * (p3 - p2);
            vel->v[c] = db * de / dur;
        }
        return;
    }
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

// Letters follow their text's fill, stroke and stroke width (docs/text.md) until
// overridden: from the start of the letter's own first track, or for good once a
// setter wrote the letter's base value. While a property follows, sampling it
// samples the text's.
static bool follows_text(ani_prop_ref ref, ani_prop_ref *text) {
    if (ref.kind != ANI_TARGET_OBJ || ref.obj->kind != AN_KIND_GLYPH) return false;
    if (ref.prop != AN_PROP_FILL && ref.prop != AN_PROP_STROKE && ref.prop != AN_PROP_STROKE_WIDTH) return false;
    if (ref.obj->own_base & (1u << ref.prop)) return false;
    *text = ani_obj_prop(ref.obj->u.glyph.text, ref.prop);
    return true;
}

ani_value ani_sample_vel(ani_prop_ref ref, ani_time at, ani_value *velocity) {
    const ani_prop_info *pi = ani_prop_info_of(ref.prop);
    const ani_track *t;
    ani_value val, vel;
    const sample_kind k = sample_interp(channel(ref), pi->ncomp, at, &t, &val, &vel);
    if (velocity) *velocity = vel;
    ani_prop_ref text;
    if (k == AT_BASE && follows_text(ref, &text)) return ani_sample_vel(text, at, velocity);
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

static int64_t record_track(an_scene *s, ani_prop_ref ref, int64_t start, int64_t duration, an_ease ease,
                            ani_value value, bool relative, const ani_value *from, const double *arc_height,
                            double lift) {
    const ani_prop_info *pi = ani_prop_info_of(ref.prop);
    ani_track t = { .start = start, .seq = ++s->seq, .ease = ease };
    ani_value inherited = { { 0 } }; // the channel's velocity at the start (arcs inherit it)

    if (from) {
        // explicit start (e.g. an_draw always starts at 0): a deliberate jump, at rest
        t.from_natural = *from;
        t.from = to_interp(pi, *from);
        t.v0 = (ani_value){ { 0 } };
    } else {
        // from and velocity: the channel as it stands, at the new track's start
        const ani_track *w;
        ani_value vel;
        ani_prop_ref text;
        switch (sample_interp(channel(ref), pi->ncomp, ani_tick_time(start), &w, &t.from, &vel)) {
        case AT_BASE:
            // a letter taking over from its text starts from the text's value (and
            // momentum) at that moment, so there's no jump
            if (follows_text(ref, &text)) {
                ani_value tv;
                t.from_natural = ani_sample_vel(text, ani_tick_time(start), &tv);
                vel = (ani_value){ { 0 } };
                if (ease.kind == AN_EASE_KIND_SPRING) vel = tv;
            } else {
                t.from_natural = ani_base_get(ref);
            }
            t.from = to_interp(pi, t.from_natural);
            break;
        case AT_START: t.from_natural = w->from_natural; break;
        case AT_END: t.from_natural = w->to_natural; break;
        case AT_RUNNING: t.from_natural = to_natural(ref.prop, pi, t.from); break;
        }
        // only springs carry momentum; eased tracks start at rest by definition
        t.v0 = (ease.kind == AN_EASE_KIND_SPRING) ? vel : (ani_value){ { 0 } };
        inherited = vel;
    }

    if (relative && pi->space == ANI_SPACE_LINEAR) { // (log space: relative isn't offered)
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

    if (arc_height) {
        // P1 = P0 + n c, P2 = P3 + n c, c = 4 h / 3 (peak h); n: the chord's normal
        // pointing up the screen (y < 0), or -x for a vertical chord
        const double dx = t.to.v[0] - t.from.v[0], dy = t.to.v[1] - t.from.v[1];
        const double len = sqrt(dx * dx + dy * dy);
        double nx = 0.0, ny = -1.0;
        if (len > 0.0) {
            nx = dy / len;
            ny = -dx / len;
            if (ny > 0.0 || (ny == 0.0 && nx > 0.0)) {
                nx = -nx;
                ny = -ny;
            }
        }
        const double c = 4.0 * *arc_height / 3.0;
        t.arc = true;
        t.c1 = (ani_value){ { t.from.v[0] + nx * c, t.from.v[1] + ny * c } };
        t.c2 = (ani_value){ { t.to.v[0] + nx * c, t.to.v[1] + ny * c } };
        // take-over: a linear ease (u'(0) = 1) inherits the channel's velocity as
        // the start tangent; eased tracks start at rest (docs/timeline.md)
        if (ease.kind == AN_EASE_KIND_LINEAR && duration > 0) {
            const double secs = (double)duration / (double)ANI_TICKS_PER_SEC;
            t.c1.v[0] += inherited.v[0] * secs / 3.0;
            t.c1.v[1] += inherited.v[1] * secs / 3.0;
        }
        t.v0 = (ani_value){ { 0 } };
        if (lift > 0.0) {
            t.lift = lift < 0.5 ? lift : 0.499;
            t.height = *arc_height;
            t.nx = nx;
            t.ny = ny;
        }
    }

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
        if (ref.kind == ANI_TARGET_OBJ) ref.obj->own_base |= 1u << ref.prop; // letters: overridden
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
an_anim an_move_arc(an_obj *o, an_vec2 to, float height, const an_anim_opts *op) {
    an_anim a = anim(o, AN_PROP_POS, false, to.x, to.y, 0, 0, op);
    a.arc = true;
    a.arc_height = height;
    return a;
}
an_anim an_draw(an_obj *o, const an_anim_opts *op) {
    an_anim a = anim(o, AN_PROP_DRAW, false, 1, 0, 0, 0, op);
    a.has_from = true; // from[] is zero: always starts at 0
    return a;
}

int64_t ani_record(an_scene *s, ani_prop_ref ref, int64_t start, int64_t duration, an_ease ease, ani_value value,
                   bool relative, const ani_value *from) {
    return record_track(s, ref, start, duration, ease, value, relative, from, NULL, 0.0);
}

int64_t ani_record_arc(an_scene *s, ani_prop_ref ref, int64_t start, int64_t duration, an_ease ease, ani_value to,
                       double height) {
    if (ease.kind == AN_EASE_KIND_SPRING) {
        static bool warned = false; // a script-time message, not render state
        if (!warned) fprintf(stderr, "anown: an_move_arc: springs aren't supported on arcs; using ease in-out\n");
        warned = true;
        ease = AN_EASE_IN_OUT;
        if (duration < 0) duration = ANI_TICKS_PER_SEC;
    }
    return record_track(s, ref, start, duration, ease, to, false, NULL, &height, 0.0);
}

int64_t ani_record_lift(an_scene *s, ani_prop_ref ref, int64_t start, int64_t duration, an_ease ease, ani_value to,
                        double height, double lift) {
    if (ease.kind == AN_EASE_KIND_SPRING) ease = AN_EASE_IN_OUT;
    return record_track(s, ref, start, duration, ease, to, false, NULL, &height, lift > 1e-6 ? lift : 1e-6);
}

// ---- camera shake (docs/camera.md) ---------------------------------------------------

typedef struct ani_shake {
    int64_t start, duration; // ticks
    float intensity;         // screen px at full trauma
} ani_shake;

static int64_t record_shake(an_scene *s, int64_t start, float seconds, float intensity) {
    const int64_t duration = ani_ticks(seconds);
    if (duration <= 0 || !(intensity > 0.0f)) return start;
    if (s->nshakes == s->shake_cap) {
        const int cap = s->shake_cap ? s->shake_cap * 2 : 8;
        ani_shake *n = ani_arena_alloc(&s->arena, sizeof *n * (size_t)cap, _Alignof(ani_shake));
        if (!n) return start;
        if (s->nshakes) memcpy(n, s->shakes, sizeof *n * (size_t)s->nshakes);
        s->shakes = n;
        s->shake_cap = cap;
    }
    s->shakes[s->nshakes++] = (ani_shake){ .start = start, .duration = duration, .intensity = intensity };
    if (start + duration > s->end) s->end = start + duration;
    return start + duration;
}

an_anim an_shake(an_scene *s, float intensity, const an_anim_opts *op) {
    an_anim a = anim(NULL, AN_PROP_CAMERA_SHAKE, false, intensity, 0, 0, 0, op);
    a.scene = s;
    return a;
}

// Smooth value noise for event e, channel c, at k samples since its trigger.
static double shake_noise(uint64_t seed, int e, int c, double k) {
    const double i = floor(k), f = k - i, w = f * f * (3.0 - 2.0 * f);
    const uint64_t stream = 0x5EA4E000ull + 2ull * (uint64_t)e + (uint64_t)c;
    const double h0 = an_hash_float(seed, stream, (uint64_t)i), h1 = an_hash_float(seed, stream, (uint64_t)i + 1);
    return (h0 + (h1 - h0) * w) * 2.0 - 1.0;
}

an_vec2 ani_shake_at(const an_scene *s, ani_time at) {
    double x = 0.0, y = 0.0;
    for (int e = 0; e < s->nshakes; e++) {
        const ani_shake *sh = &s->shakes[e];
        // exact: elapsed and duration in units of 1 / (den x 1e6) s
        const int64_t el = at.num * ANI_TICKS_PER_SEC - sh->start * at.den, dur = sh->duration * at.den;
        if (el < 0 || el >= dur) continue; // outside the event: exactly nothing
        const double u = (double)el / (double)dur, trauma = 1.0 - u;
        const double amp = (double)sh->intensity * trauma * trauma; // trauma exponent 2
        const double k = (double)el / ((double)at.den * (double)ANI_TICKS_PER_SEC) * 15.0; // 15 Hz
        x += amp * shake_noise(s->seed, e, 0, k);
        y += amp * shake_noise(s->seed, e, 1, k);
    }
    return (an_vec2){ (float)x, (float)y };
}

// ---- verbs (docs/camera.md) -------------------------------------------------------------

// Seconds from a spring's start (at rest, d0 != 0) until it first reaches its
// target: d(t) = d0 e^(-zwt) (cos wd t + (zw / wd) sin wd t) = 0 at
// t* = (pi - atan(wd / (zw))) / wd. A critically damped spring never crosses: -1.
double ani_spring_first_arrival(an_ease e) {
    const ani_spring sp = ani_spring_params(e);
    if (sp.zeta >= 1.0) return -1.0;
    const double wd = sp.omega * sqrt(1.0 - sp.zeta * sp.zeta);
    return (3.14159265358979323846 - atan(wd / (sp.zeta * sp.omega))) / wd;
}

static int64_t record_verb(an_scene *s, const an_anim *a, int64_t start) {
    an_obj *o = a->obj;
    const an_verb_opts *v = &a->verb_opts;
    // the pivot, as a step at the start: a later default-pivot verb puts the center back
    const an_vec2 pivot = v->custom_pivot ? v->pivot : o->vpivot; // base: the center
    // an entrance: hidden from creation until the verb starts (instant opacity
    // changes at the object's birth and at the start; no fade, so touching
    // shapes don't seam)
    const bool enter = v->enter == AN_ON || (v->enter == AN_AUTO && a->verb == AN_VERB_POP);
    if (enter) {
        const int64_t birth = o->birth < start ? o->birth : start;
        ani_record(s, ani_obj_prop(o, AN_PROP_OPACITY), birth, 0, AN_EASE_LINEAR, (ani_value){ { 0 } }, false, NULL);
        ani_record(s, ani_obj_prop(o, AN_PROP_OPACITY), start, 0, AN_EASE_LINEAR, (ani_value){ { 1 } }, false, NULL);
    }
    ani_record(s, ani_obj_prop(o, AN_PROP_VERB_PIVOT), start, 0, AN_EASE_LINEAR, (ani_value){ { pivot.x, pivot.y } },
               false, NULL);
    an_ease ease;
    ani_value from;
    float rot_from = 0.0f;
    switch (a->verb) {
    case AN_VERB_SLAP: {
        const double k = v->amount > 0.0f ? v->amount : 2.5;
        ease = an_spring(0.45f, 0.22f);
        from = (ani_value){ { k, k } };
        rot_from = -0.06f;
        break;
    }
    case AN_VERB_POP:
        ease = an_spring(0.5f, 0.3f);
        from = (ani_value){ { 0, 0 } };
        break;
    default: { // squash and stretch
        const double k = 1.0 + (v->amount > 0.0f ? v->amount : 0.3);
        ease = an_spring(0.35f, 0.3f);
        from = (ani_value){ { k, 1.0 / k } };
        break;
    }
    }
    const int64_t duration = v->duration > 0.0f ? ani_ticks(v->duration) : -1;
    const int64_t end = ani_record(s, ani_obj_prop(o, AN_PROP_VERB_SCALE), start, duration, ease, (ani_value){ { 1, 1 } },
                                   false, &from);
    if (rot_from != 0.0f) {
        const ani_value rf = { { rot_from } };
        ani_record(s, ani_obj_prop(o, AN_PROP_VERB_ROTATION), start, duration, ease, (ani_value){ { 0 } }, false, &rf);
    }
    if (a->verb == AN_VERB_SLAP && v->impact_shake > 0.0f) {
        // impact: the first frame at or after the moment the scale first reaches 1
        const double tstar = ani_spring_first_arrival(ease);
        double t_imp = tstar >= 0.0 ? (double)start + tstar * (double)ANI_TICKS_PER_SEC : (double)end;
        if (t_imp > (double)end) t_imp = (double)end;
        const int64_t frame = (int64_t)ceil(t_imp * (double)s->fps / (double)ANI_TICKS_PER_SEC - 1e-9);
        const int64_t tick = frame * ANI_TICKS_PER_SEC / s->fps; // floor: reached at that frame, not before
        record_shake(s, tick, v->impact_shake_duration > 0.0f ? v->impact_shake_duration : 0.5f, v->impact_shake);
    }
    return end;
}

static an_anim verb(an_obj *o, an_verb_kind k, const an_verb_opts *opts) {
    an_anim a = { .obj = o, .verb = k };
    if (opts) a.verb_opts = *opts;
    return a;
}
an_anim an_slap(an_obj *o, const an_verb_opts *opts) { return verb(o, AN_VERB_SLAP, opts); }
an_anim an_pop(an_obj *o, const an_verb_opts *opts) { return verb(o, AN_VERB_POP, opts); }
an_anim an_squash_stretch(an_obj *o, const an_verb_opts *opts) { return verb(o, AN_VERB_SQUASH, opts); }

// Record `a` starting at `start`; returns its end (or start if it can't run).
int64_t ani_record_anim(an_scene *s, const an_anim *a, int64_t start) {
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
    if (a->prop == AN_PROP_CAMERA_SHAKE) return record_shake(s, start, a->opts.duration > 0.0f ? a->opts.duration : 0.5f, a->value[0]);
    if (a->verb == AN_VERB_SWAP && a->obj) return ani_array_record_swap(s, a, start);
    if (a->verb == AN_VERB_CODE_GOTO && a->obj) return ani_code_record_goto(s, a, start);
    if (a->verb == AN_VERB_CALLOUT_DRAW && a->obj) return ani_callout_record_draw(s, a, start);
    if (a->verb == AN_VERB_CELL_STATE && a->obj) return ani_array_record_state(s, a, start);
    if (a->verb != AN_VERB_NONE && a->obj) return record_verb(s, a, start);
    const bool spring = a->opts.ease.kind == AN_EASE_KIND_SPRING;
    int64_t duration = ani_ticks(a->opts.duration);
    if (duration == 0) duration = spring ? -1 : ANI_TICKS_PER_SEC;
    const ani_value v = { { a->value[0], a->value[1], a->value[2], a->value[3] } };
    const ani_value from = { { a->from[0], a->from[1], a->from[2], a->from[3] } };
    if (a->arc && a->prop == AN_PROP_POS) return ani_record_arc(s, ref, start, duration, a->opts.ease, v, a->arc_height);
    return ani_record(s, ref, start, duration, a->opts.ease, v, a->relative,
                      a->has_from ? &from : NULL);
}

static an_handle handle(an_scene *s, int64_t start, int64_t end) {
    return (an_handle){ .scene = s, .start = (double)start / (double)ANI_TICKS_PER_SEC,
                        .end = (double)end / (double)ANI_TICKS_PER_SEC };
}

an_handle an_play(an_scene *s, an_anim a) {
    if (!s) return (an_handle){ 0 };
    const int64_t start = s->cursor;
    const int64_t end = ani_record_anim(s, &a, start);
    if (end > s->cursor) s->cursor = end;
    return handle(s, start, end > start ? end : start);
}

an_handle an_play_async(an_scene *s, an_anim a) {
    if (!s) return (an_handle){ 0 };
    const int64_t end = ani_record_anim(s, &a, s->cursor);
    return handle(s, s->cursor, end > s->cursor ? end : s->cursor);
}

void an_wait_until(an_handle h) {
    an_scene *s = h.scene;
    if (!s) return;
    const int64_t end = llround(h.end * (double)ANI_TICKS_PER_SEC); // exact: ticks came from integers
    if (end > s->cursor) s->cursor = end;
}

void an_wait(an_scene *s, float seconds) {
    if (s) s->cursor += ani_ticks(seconds);
}

an_handle an_par(an_scene *s, int n, const an_anim *anims) {
    return an_stagger(s, n, anims, 0.0f);
}

an_handle an_stagger(an_scene *s, int n, const an_anim *anims, float gap_seconds) {
    if (!s || !anims) return (an_handle){ 0 };
    const int64_t gap = ani_ticks(gap_seconds), start = s->cursor;
    int64_t latest = s->cursor;
    for (int i = 0; i < n; i++) {
        const int64_t end = ani_record_anim(s, &anims[i], s->cursor + (int64_t)i * gap);
        if (end > latest) latest = end;
    }
    s->cursor = latest;
    return handle(s, start, latest);
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

static an_obj_state obj_state_at(const an_obj *o, ani_time at);

an_obj_state an_obj_get(const an_obj *o) {
    if (!o) return (an_obj_state){ 0 };
    return obj_state_at(o, ani_frame_time(o->scene, o->scene->frame));
}

an_obj_state an_obj_at_cursor(const an_obj *o) {
    if (!o) return (an_obj_state){ 0 };
    return obj_state_at(o, ani_tick_time(o->scene->cursor));
}

static an_obj_state obj_state_at(const an_obj *o, ani_time at) {
    an_obj *m = (an_obj *)o; // sampling reads only; the handle type isn't const
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

// ---- camera (docs/camera.md) ----------------------------------------------------------

static an_anim scene_anim(an_scene *s, an_prop p, float a, float b, const an_anim_opts *op) {
    an_anim x = anim(NULL, p, false, a, b, 0, 0, op);
    x.scene = s;
    return x;
}
an_anim an_camera_move_to(an_scene *s, an_vec2 pos, const an_anim_opts *op) { return scene_anim(s, AN_PROP_CAMERA_POS, pos.x, pos.y, op); }
an_anim an_camera_zoom_to(an_scene *s, float z, const an_anim_opts *op) { return scene_anim(s, AN_PROP_CAMERA_ZOOM, z > 1e-6f ? z : 1e-6f, 0, op); }
an_anim an_camera_rotate_to(an_scene *s, float r, const an_anim_opts *op) { return scene_anim(s, AN_PROP_CAMERA_ROTATION, r, 0, op); }
void an_camera_set_pos(an_scene *s, an_vec2 p) {
    if (s) ani_set(ani_scene_prop(s, AN_PROP_CAMERA_POS), (ani_value){ { p.x, p.y } });
}
void an_camera_set_zoom(an_scene *s, float z) {
    if (s) ani_set(ani_scene_prop(s, AN_PROP_CAMERA_ZOOM), (ani_value){ { z > 1e-6f ? z : 1e-6f } });
}
void an_camera_set_rotation(an_scene *s, float r) {
    if (s) ani_set(ani_scene_prop(s, AN_PROP_CAMERA_ROTATION), (ani_value){ { r } });
}

ani_camera ani_camera_at(an_scene *s, ani_time at) {
    const ani_value p = ani_sample(ani_scene_prop(s, AN_PROP_CAMERA_POS), at);
    ani_camera c = {
        .pos = { (float)p.v[0], (float)p.v[1] },
        .zoom = (float)ani_sample(ani_scene_prop(s, AN_PROP_CAMERA_ZOOM), at).v[0],
        .rot = (float)ani_sample(ani_scene_prop(s, AN_PROP_CAMERA_ROTATION), at).v[0],
    };
    if (s->nshakes) c.shake = ani_shake_at(s, at); // only then: no float work at all without shakes
    return c;
}

double ani_obj_area_scale_at(an_obj *o, ani_time at) {
    double det = ani_sample(ani_scene_prop(o->scene, AN_PROP_CAMERA_ZOOM), at).v[0];
    det *= det;
    for (an_obj *p = o; p && p->parent; p = p->parent) {
        const ani_value sc = ani_sample(ani_obj_prop(p, AN_PROP_SCALE), at);
        const ani_value vs = ani_sample(ani_obj_prop(p, AN_PROP_VERB_SCALE), at);
        det *= sc.v[0] * sc.v[1] * vs.v[0] * vs.v[1];
    }
    return sqrt(fabs(det));
}

an_camera_state an_camera_get(const an_scene *s) {
    if (!s) return (an_camera_state){ 0 };
    const ani_camera c = ani_camera_at((an_scene *)s, ani_frame_time(s, s->frame)); // sampling reads only
    return (an_camera_state){ .pos = c.pos, .zoom = c.zoom, .rotation = c.rot, .shake = c.shake };
}

double ani_scene_chaos_at(an_scene *s, ani_time at) {
    return ani_sample(ani_scene_prop(s, AN_PROP_CHAOS), at).v[0];
}

double ani_obj_chaos_at(an_obj *o, ani_time at) {
    double c = ani_scene_chaos_at(o->scene, at);
    // of the world transform: the camera zoom squared times the scales' determinants
    const double zoom = ani_sample(ani_scene_prop(o->scene, AN_PROP_CAMERA_ZOOM), at).v[0];
    double det = zoom * zoom;
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
