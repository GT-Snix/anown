#include "ease_internal.h"

#include <math.h>

#define ANI_PI_D 3.14159265358979323846

// ---- normalized easings ---------------------------------------------------------

double ani_ease_value(an_ease_kind kind, double u) {
    if (u <= 0.0) return 0.0;
    if (u >= 1.0) return 1.0;
    switch (kind) {
    case AN_EASE_KIND_LINEAR:
        return u;
    case AN_EASE_KIND_IN_OUT:
    default:
        if (u < 0.5) return 4.0 * u * u * u;
        {
            const double k = -2.0 * u + 2.0;
            return 1.0 - k * k * k / 2.0;
        }
    }
}

double ani_ease_deriv(an_ease_kind kind, double u) {
    if (u < 0.0 || u > 1.0) return 0.0;
    switch (kind) {
    case AN_EASE_KIND_LINEAR:
        return 1.0;
    case AN_EASE_KIND_IN_OUT:
    default:
        if (u < 0.5) return 12.0 * u * u;
        {
            const double k = -2.0 * u + 2.0;
            return 3.0 * k * k;
        }
    }
}

// ---- springs ----------------------------------------------------------------------

ani_spring ani_spring_params(an_ease e) {
    double zeta = e.damping;
    if (!(zeta >= 0.01)) zeta = 0.01; // also catches NaN
    if (zeta > 1.0) zeta = 1.0;
    double response = e.response > 0.0f ? (double)e.response : 0.5;
    return (ani_spring){ .zeta = zeta, .omega = 2.0 * ANI_PI_D / response };
}

void ani_spring_eval(ani_spring sp, double d0, double v0, double t, double *d, double *dv) {
    const double w = sp.omega, z = sp.zeta;
    if (z >= 1.0) {
        const double k = v0 + w * d0;
        const double ex = exp(-w * t);
        *d = (d0 + k * t) * ex;
        *dv = (v0 - w * t * k) * ex;
    } else {
        const double wd = w * sqrt(1.0 - z * z);
        const double B = (v0 + z * w * d0) / wd;
        const double ex = exp(-z * w * t);
        const double c = cos(wd * t), s = sin(wd * t);
        *d = ex * (d0 * c + B * s);
        *dv = ex * (-z * w * (d0 * c + B * s) + (-d0 * wd * s + B * wd * c));
    }
}

double ani_spring_settle(ani_spring sp, double d0, double v0, double tol) {
    const double w = sp.omega, z = sp.zeta;
    if (!(tol > 0.0)) tol = 1e-9;
    if (z < 1.0) {
        // |d(t)| <= A e^(-z w t) with A = sqrt(d0^2 + B^2)
        const double wd = w * sqrt(1.0 - z * z);
        const double B = (v0 + z * w * d0) / wd;
        const double A = sqrt(d0 * d0 + B * B);
        if (A <= tol) return 0.0;
        return log(A / tol) / (z * w);
    }
    // critical: |d(t)| <= f(t) = (a + b t) e^(-w t), a = |d0|, b = |v0 + w d0|
    const double a = fabs(d0), b = fabs(v0 + w * d0);
    if (b == 0.0) return a <= tol ? 0.0 : log(a / tol) / w;
    // f rises until t_peak = 1/w - a/b, then falls monotonically
    double lo = 1.0 / w - a / b;
    if (lo < 0.0) lo = 0.0;
    #define F(t) ((a + b * (t)) * exp(-w * (t)))
    if (F(lo) <= tol) return 0.0; // even the peak is under tol
    double hi = lo + 1.0 / w;
    while (F(hi) > tol) hi = lo + (hi - lo) * 2.0;
    for (int i = 0; i < 100; i++) {
        const double mid = 0.5 * (lo + hi);
        if (F(mid) > tol) lo = mid;
        else hi = mid;
    }
    #undef F
    return hi;
}

// ---- OKLab (Björn Ottosson, https://bottosson.github.io/posts/oklab/) -------------

static double srgb_to_linear(double c) {
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static double linear_to_srgb(double c) {
    return c <= 0.0031308 ? 12.92 * c : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

// linear_to_srgb(1.0) is a hair off 1.0 in floating point; outputs are [0,1]
static double linear_to_srgb01(double c) {
    const double v = linear_to_srgb(c);
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

ani_lab ani_srgb_to_oklab(double r, double g, double b) {
    r = srgb_to_linear(r);
    g = srgb_to_linear(g);
    b = srgb_to_linear(b);
    const double l = cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const double m = cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const double s = cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    return (ani_lab){
        .L = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
        .a = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
        .b = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s,
    };
}

// Linear RGB (unclamped) from OKLab.
static void oklab_to_linear(ani_lab c, double *r, double *g, double *b) {
    double l = c.L + 0.3963377774 * c.a + 0.2158037573 * c.b;
    double m = c.L - 0.1055613458 * c.a - 0.0638541728 * c.b;
    double s = c.L - 0.0894841775 * c.a - 1.2914855480 * c.b;
    l = l * l * l;
    m = m * m * m;
    s = s * s * s;
    *r = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
    *g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
    *b = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
}

static double clamp01(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }

void ani_oklab_to_srgb_raw(ani_lab lab, double *r, double *g, double *b) {
    double lr, lg, lb;
    oklab_to_linear(lab, &lr, &lg, &lb);
    // sign-preserving so out-of-gamut values stay visible to callers
    *r = lr < 0 ? -linear_to_srgb(-lr) : linear_to_srgb(lr);
    *g = lg < 0 ? -linear_to_srgb(-lg) : linear_to_srgb(lg);
    *b = lb < 0 ? -linear_to_srgb(-lb) : linear_to_srgb(lb);
}

static int in_gamut(ani_lab c) {
    const double eps = 1e-9;
    double r, g, b;
    oklab_to_linear(c, &r, &g, &b);
    return r >= -eps && r <= 1 + eps && g >= -eps && g <= 1 + eps && b >= -eps && b <= 1 + eps;
}

void ani_oklab_to_srgb(ani_lab lab, double *r, double *g, double *b) {
    // Lightness outside [0,1] (spring overshoot) has no sRGB color with any
    // chroma: clamp it to black or white.
    if (!(lab.L > 0.0)) {
        *r = *g = *b = 0.0;
        return;
    }
    if (lab.L >= 1.0) {
        *r = *g = *b = 1.0;
        return;
    }
    if (!in_gamut(lab)) {
        // keep L and hue, bisect on chroma scale; k = 0 (gray) is always in gamut
        double lo = 0.0, hi = 1.0;
        for (int i = 0; i < 40; i++) {
            const double mid = 0.5 * (lo + hi);
            if (in_gamut((ani_lab){ lab.L, lab.a * mid, lab.b * mid })) lo = mid;
            else hi = mid;
        }
        lab.a *= lo;
        lab.b *= lo;
    }
    double lr, lg, lb;
    oklab_to_linear(lab, &lr, &lg, &lb);
    *r = linear_to_srgb01(clamp01(lr));
    *g = linear_to_srgb01(clamp01(lg));
    *b = linear_to_srgb01(clamp01(lb));
}
