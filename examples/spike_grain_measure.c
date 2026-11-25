// spike_grain_measure: how much paper grain survives encoding (bitrate study,
// scripts/bitrate-study.sh). Reads two raw 8-bit grey streams of the same
// paper-only patch (W x H x N frames): the source frames and the decoded video.
// Per frame, a high-pass (pixel minus its 5x5 box mean, interior only) of each;
// reports, averaged over frames:
//   retained  = std(high-pass of decoded) / std(high-pass of source)
//   corr      = correlation of the two high-passes (1: the same grain; codec
//               noise or smoothing lowers it)
//   temporal  = std over the patch of (frame - previous frame), decoded / source,
//               only across frames where the source changes (the 12 Hz grain steps)
//
//   spike_grain_measure W H SOURCE.gray DECODED.gray
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void highpass(const unsigned char *in, double *out, int w, int h) {
    for (int y = 2; y < h - 2; y++) {
        for (int x = 2; x < w - 2; x++) {
            double m = 0;
            for (int dy = -2; dy <= 2; dy++)
                for (int dx = -2; dx <= 2; dx++) m += in[(y + dy) * w + x + dx];
            out[y * w + x] = in[y * w + x] - m / 25.0;
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: spike_grain_measure W H SOURCE.gray DECODED.gray\n");
        return 2;
    }
    const int w = atoi(argv[1]), h = atoi(argv[2]);
    FILE *fa = fopen(argv[3], "rb"), *fb = fopen(argv[4], "rb");
    if (!fa || !fb || w < 8 || h < 8) return 2;
    const size_t n = (size_t)w * h;
    unsigned char *a = malloc(n), *b = malloc(n), *pa = malloc(n), *pb = malloc(n);
    double *ha = calloc(n, sizeof *ha), *hb = calloc(n, sizeof *hb);
    double ret = 0, cor = 0, tmp = 0;
    int frames = 0, steps = 0;
    while (fread(a, 1, n, fa) == n && fread(b, 1, n, fb) == n) {
        highpass(a, ha, w, h);
        highpass(b, hb, w, h);
        double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
        int k = 0;
        for (int y = 2; y < h - 2; y++) {
            for (int x = 2; x < w - 2; x++) {
                const double u = ha[y * w + x], v = hb[y * w + x];
                sa += u; sb += v; saa += u * u; sbb += v * v; sab += u * v;
                k++;
            }
        }
        const double va = saa / k - (sa / k) * (sa / k), vb = sbb / k - (sb / k) * (sb / k);
        const double cv = sab / k - (sa / k) * (sb / k);
        ret += sqrt(vb / va);
        cor += cv / sqrt(va * vb);
        if (frames > 0) {
            double da = 0, db = 0;
            for (size_t i = 0; i < n; i++) {
                const double u = (double)a[i] - pa[i], v = (double)b[i] - pb[i];
                da += u * u;
                db += v * v;
            }
            if (da / n > 1.0) { // the source changed: a grain step
                tmp += sqrt(db / da);
                steps++;
            }
        }
        for (size_t i = 0; i < n; i++) { pa[i] = a[i]; pb[i] = b[i]; }
        frames++;
    }
    if (!frames) return 1;
    printf("retained %.3f corr %.3f temporal %.3f frames %d steps %d\n", ret / frames, cor / frames,
           steps ? tmp / steps : 0.0, frames, steps);
    return 0;
}
