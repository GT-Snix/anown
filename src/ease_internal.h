// Internal: easing curves, analytic springs, OKLab. Pure functions, no state.
#ifndef ANOWN_EASE_INTERNAL_H
#define ANOWN_EASE_INTERNAL_H

#include "anown.h"

// Normalized easing (linear, in-out) at progress u in [0,1], and its derivative.
double ani_ease_value(an_ease_kind kind, double u);
double ani_ease_deriv(an_ease_kind kind, double u);

// Spring parameters after clamping (zeta in [0.01, 1], response > 0).
typedef struct ani_spring {
    double zeta;  // damping ratio
    double omega; // natural angular frequency, 2*pi / response
} ani_spring;

ani_spring ani_spring_params(an_ease e);

// Displacement d(t) = x(t) - target and its derivative, from d(0) = d0,
// d'(0) = v0, at t seconds. Exact closed form.
void ani_spring_eval(ani_spring sp, double d0, double v0, double t, double *d, double *dv);

// Smallest t >= 0 after which the error envelope stays <= tol, for one
// component. 0 if it already is at t = 0.
double ani_spring_settle(ani_spring sp, double d0, double v0, double tol);

// ---- OKLab ----------------------------------------------------------------

typedef struct ani_lab {
    double L, a, b;
} ani_lab;

ani_lab ani_srgb_to_oklab(double r, double g, double b);
// Unclamped conversion back (may be outside [0,1]).
void ani_oklab_to_srgb_raw(ani_lab lab, double *r, double *g, double *b);
// Gamut-clamped: keeps L and hue, reduces chroma until in sRGB, then clips.
void ani_oklab_to_srgb(ani_lab lab, double *r, double *g, double *b);

#endif
