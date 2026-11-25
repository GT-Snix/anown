// spike_frame_bench: frame time for 200 strokes + 50 filled shapes, offscreen at
// 1920x1080 with 4x MSAA. Used for before/after numbers when the compositing
// architecture changes (docs/layers.md).
//
// Reports per frame: CPU tessellation alone, and the full frame (tessellate,
// upload, draw, MSAA resolve, readback). The readback waits for the GPU, so the
// full number includes GPU time; readback alone is ~1.2 ms of it.
//
//   spike_frame_bench [scenario] [--profile]
// --profile also splits every measured frame into phases (tessellate, upload,
// encode + commit, GPU render, the readback blit, the copy out) and prints the
// median of each phase and the split of the median and p99 frames.
#include "anown.h"
#include "offscreen_internal.h"
#include "render_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <mach/mach.h>

// Timings taken on a busy machine are noise (docs/layers.md, 5A/5C results), so the
// bench refuses to report unless the CPU is quiet, checked before measuring and
// again after (things can start during the run). Exit code 3.
//
// Measure: CPU idle over one second from the kernel's tick counters
// (HOST_CPU_LOAD_INFO, summed over all cores), turned into "busy cores" =
// (1 - idle) x core count. The allowance is MAX_BUSY_CORES of OTHER work, the same
// on any core count: the bench needs one core, and a load-average threshold would
// mean something different on 10 cores than on 4. On this M4 (10 cores) 1.5 busy
// cores is 85% idle. The GPU isn't measured: a GPU-heavy app can still skew results.
#define MAX_BUSY_CORES 1.5

static bool cpu_ticks(unsigned long long *busy, unsigned long long *total) {
    host_cpu_load_info_data_t info;
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, (host_info_t)&info, &count) != KERN_SUCCESS) return false;
    const unsigned long long user = info.cpu_ticks[CPU_STATE_USER], sys = info.cpu_ticks[CPU_STATE_SYSTEM],
                             nice = info.cpu_ticks[CPU_STATE_NICE], idle = info.cpu_ticks[CPU_STATE_IDLE];
    *busy = user + sys + nice;
    *total = user + sys + nice + idle;
    return true;
}

static bool machine_quiet(const char *when) {
    unsigned long long b0, t0, b1, t1;
    int ncpu = 0;
    size_t len = sizeof ncpu;
    double load[1] = { -1 };
    getloadavg(load, 1);
    if (sysctlbyname("hw.ncpu", &ncpu, &len, NULL, 0) != 0 || ncpu <= 0 || !cpu_ticks(&b0, &t0)) {
        fprintf(stderr, "spike_frame_bench: can't read CPU statistics; refusing to report timings\n");
        return false;
    }
    sleep(1);
    if (!cpu_ticks(&b1, &t1) || t1 <= t0) {
        fprintf(stderr, "spike_frame_bench: can't read CPU statistics; refusing to report timings\n");
        return false;
    }
    const double busy_frac = (double)(b1 - b0) / (double)(t1 - t0);
    const double busy_cores = busy_frac * ncpu;
    if (busy_cores > MAX_BUSY_CORES) {
        fprintf(stderr, "spike_frame_bench: %.2f of %d cores busy %s (%.0f%% idle; 1-minute load %.2f); the limit is "
                        "%.1f busy cores. Refusing to report timings (they would be noise). Close what's busy and re-run.\n",
                busy_cores, ncpu, when, 100.0 * (1.0 - busy_frac), load[0], MAX_BUSY_CORES);
        return false;
    }
    printf("machine check %s: %.2f of %d cores busy (%.0f%% idle), 1-minute load %.2f\n", when, busy_cores, ncpu,
           100.0 * (1.0 - busy_frac), load[0]);
    return true;
}

static int cmp_double(const void *a, const void *b) {
    const double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static int g_layers_this_frame(const ani_renderer *r) { return r->layers_rendered + r->layers_fallback; }

static double uptime_s(void) { return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) * 1e-9; }

// one frame's phases, ms
enum { PH_TESS, PH_UPLOAD, PH_PREP_REST, PH_ENCODE, PH_COMMIT, PH_GPU_QUEUE, PH_GPU, PH_GAP, PH_BLIT, PH_WAKE, PH_COPY, PH_COUNT };
static const char *ph_names[PH_COUNT] = {
    "tessellate", "upload (vertex memcpy)", "prepare, rest (layers)", "encode main pass", "sg_commit",
    "commit -> GPU start", "GPU render + resolve", "GPU end -> blit start", "readback blit (GPU)",
    "blit end -> wait returns", "copy out (memcpy 8.3 MB)" };

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

// scenario 0: everything opaque (direct path)
// scenario 1: the 50 shapes fade in together over 2 s (50 isolated layers per frame)
// scenario 2: everything in one group fading in over the whole timeline (1 big layer)
// scenario 5: a 30-line x 60-column code block (24 px, ~70% non-space), line
// numbers, a highlight bar stepping down one line every 0.2 s, for 6 s
static an_scene *build_code(void) {
    an_scene *s = an_scene_create(&(an_scene_desc){ .fps = 60, .seed = 7 });
    static char code[30 * 61 + 1];
    an_rng *rng = an_scene_rng(s);
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789()[]{};=+-*<>_,.";
    char *p = code;
    for (int l = 0; l < 30; l++) {
        for (int c = 0; c < 60; c++) {
            *p++ = an_rng_float(rng) < 0.3f ? ' ' : alphabet[an_rng_below(rng, sizeof alphabet - 1)];
        }
        *p++ = l < 29 ? '\n' : 0;
    }
    an_obj *blk = an_code(s, &(an_code_opts){ .pos = { 400, 40 }, .size = 24, .code = code, .line_numbers = true });
    for (int l = 1; l <= 30; l++) {
        an_obj_set_opacity(an_code_line(blk, l), 1.0f);
        an_wait(s, 0.2f);
        an_obj_set_opacity(an_code_line(blk, l), 0.0f);
    }
    return s;
}

static an_scene *build(int scenario) {
    if (scenario == 5) return build_code();
    // default style: chaos 1, paper grain + vignette, riso on everything with ink;
    // scenario 3 also turns hard shadows on for every object
    an_scene *s = an_scene_create(&(an_scene_desc){ .fps = 60, .seed = 7, .style = { .shadow = scenario == 3,
                                                                                             .grain_off = scenario == 4, .vignette_off = scenario == 4 } });
    an_obj *group = scenario == 2 ? an_group(s, &(an_group_opts){ .hidden = true }) : NULL;
    an_obj *shapes[50];
    an_rng *rng = an_scene_rng(s);
    an_anim draws[250];
    int nd = 0;
    // 50 filled shapes with outlines, half of them fading in (opacity < 1 for a while)
    for (int i = 0; i < 50; i++) {
        const an_vec2 p = { an_rng_range(rng, 100, 1820), an_rng_range(rng, 100, 980) };
        const an_color fill = (i % 3 == 0) ? AN_HEX(0x00C2FF) : (i % 3 == 1) ? AN_HEX(0xFF2E88) : AN_HEX(0xC6FF00);
        an_obj *o = (i % 2)
            ? an_rect(s, &(an_rect_opts){ .parent = group, .hidden = scenario == 1, .pos = p, .size = { an_rng_range(rng, 40, 220), an_rng_range(rng, 40, 160) },
                                          .fill = fill, .stroke = AN_HEX(0x16161A), .stroke_width = 6, .undrawn = true,
                                          .brush = { .taper_in = 20, .taper_out = 40, .overshoot = 0.6f } })
            : an_circle(s, &(an_circle_opts){ .parent = group, .hidden = scenario == 1, .pos = p, .radius = an_rng_range(rng, 20, 110), .fill = fill,
                                              .stroke = AN_HEX(0x16161A), .stroke_width = 6, .undrawn = true,
                                              .brush = { .taper_in = 20, .taper_out = 40 } });
        shapes[i] = o;
        draws[nd++] = an_draw(o, &(an_anim_opts){ .duration = 1.0f });
    }
    // 200 strokes, 7 cubics each
    for (int i = 0; i < 200; i++) {
        an_seg segs[7];
        float x = an_rng_range(rng, 100, 1820), y = an_rng_range(rng, 100, 980);
        segs[0] = AN_MOVE(x, y);
        for (int k = 1; k < 7; k++) {
            const float nx = x + an_rng_range(rng, -120, 120), ny = y + an_rng_range(rng, -120, 120);
            segs[k] = AN_CUBIC(x + an_rng_range(rng, -80, 80), y + an_rng_range(rng, -80, 80),
                               nx + an_rng_range(rng, -80, 80), ny + an_rng_range(rng, -80, 80), nx, ny);
            x = nx;
            y = ny;
        }
        an_obj *o = an_stroke(s, &(an_stroke_opts){
            .parent = group, .segs = segs, .seg_count = 7, .stroke = AN_HEX(0x16161A), .stroke_width = 6, .undrawn = true,
            .brush = { .taper_in = 30, .taper_out = 60, .overshoot = 0.5f } });
        draws[nd++] = an_draw(o, &(an_anim_opts){ .duration = 1.5f });
    }
    if (scenario == 1) {
        for (int i = 0; i < 50; i++) an_play_async(s, an_fade_to(shapes[i], 1, &(an_anim_opts){ .duration = 2, .ease = AN_EASE_LINEAR }));
    }
    if (scenario == 2) an_play_async(s, an_fade_to(group, 1, &(an_anim_opts){ .duration = 5.5f, .ease = AN_EASE_LINEAR }));
    an_stagger(s, nd, draws, 0.016f);
    an_wait(s, 0.5f);
    return s;
}

int main(int argc, char **argv) {
    const int scenario = argc > 1 ? atoi(argv[1]) : 0;
    const bool profile = argc > 2 && strcmp(argv[2], "--profile") == 0;
    if (!machine_quiet("before the run")) return 3;
    an_scene *s = build(scenario);
    const int64_t L = an_scene_length(s);

    void *pool = ani_pool_push();
    ani_offscreen off = { 0 };
    ani_renderer r = { 0 };
    if (!ani_gfx_setup_headless() || !ani_offscreen_make_ds(&off, 1920, 1080, 4) ||
        !ani_renderer_init(&r, SG_PIXELFORMAT_RGBA8, SG_PIXELFORMAT_DEPTH_STENCIL, 4)) {
        fprintf(stderr, "setup failed\n");
        return 1;
    }
    ani_pool_pop(pool);
    static unsigned char pixels[1920 * 1080 * 4];

    // CPU tessellation alone
    ani_mesh m;
    ani_mesh_init(&m);
    double tess = 0, tess_worst = 0;
    int max_tris = 0, max_layers = 0;
    for (int64_t f = 0; f <= L; f++) {
        const double t0 = now_ms();
        ani_tess_scene(&m, s, f);
        const double dt = now_ms() - t0;
        tess += dt;
        if (dt > tess_worst) tess_worst = dt;
        if (m.count / 3 > max_tris) max_tris = m.count / 3;
    }

    // full frames. The first WARMUP frames are excluded from the statistics; for
    // every frame we also record minor page faults and any growth of the
    // renderer's memory (mesh arrays, vertex buffer, layer pool), to see what the
    // early frames are paying for.
    enum { WARMUP = 30 };
    static double times[8192];
    static long faults[8192];
    static int grew[8192];
    static double phases[8192][PH_COUNT];
    int64_t worst_frame = 0;
    double worst = 0;
    for (int64_t f = 0; f <= L; f++) {
        const int vcap = r.mesh.cap, ccap = r.mesh.cmd_cap, lcap = r.mesh.layer_cap;
        const size_t vbuf = r.vbuf_bytes, poolb = r.pool_bytes;
        struct rusage ru0, ru1;
        getrusage(RUSAGE_SELF, &ru0);
        pool = ani_pool_push();
        const double t0 = now_ms();
        ani_renderer_prepare(&r, s, f, 1920, 1080);
        ani_offscreen_begin(&off, AN_CREAM);
        ani_renderer_draw(&r);
        sg_end_pass();
        const double t_enc = uptime_s();
        const void *cb = profile ? ani_mtl_retain_frame_cmd_buffer() : NULL;
        sg_commit();
        const double t_commit = uptime_s();
        ani_offscreen_readback(&off, pixels);
        const double dt = now_ms() - t0;
        if (profile) {
            double gs = 0, ge = 0;
            ani_mtl_cmd_buffer_times(cb, &gs, &ge);
            double *ph = phases[f];
            ph[PH_TESS] = r.prof_tess - r.prof_start;
            ph[PH_UPLOAD] = r.prof_upload - r.prof_tess;
            ph[PH_PREP_REST] = r.prof_end - r.prof_upload;
            ph[PH_ENCODE] = t_enc - r.prof_end;
            ph[PH_COMMIT] = t_commit - t_enc;
            ph[PH_GPU_QUEUE] = gs - t_commit;
            ph[PH_GPU] = ge - gs;
            ph[PH_GAP] = off.rb_gpu_start - ge;
            ph[PH_BLIT] = off.rb_gpu_end - off.rb_gpu_start;
            ph[PH_WAKE] = off.rb_waited - off.rb_gpu_end;
            ph[PH_COPY] = off.rb_copied - off.rb_waited;
            for (int k = 0; k < PH_COUNT; k++) ph[k] *= 1e3;
        }
        ani_pool_pop(pool);
        getrusage(RUSAGE_SELF, &ru1);
        times[f] = dt;
        faults[f] = ru1.ru_minflt - ru0.ru_minflt;
        grew[f] = (r.mesh.cap != vcap) | (r.mesh.cmd_cap != ccap) << 1 | (r.mesh.layer_cap != lcap) << 2 |
                  (r.vbuf_bytes != vbuf) << 3 | (r.pool_bytes != poolb) << 4;
        if (g_layers_this_frame(&r) > max_layers) max_layers = g_layers_this_frame(&r);
        if (f >= WARMUP && dt > worst) {
            worst = dt;
            worst_frame = f;
        }
    }
    static const char *names[6] = { "opaque (direct path)", "50 shapes fading together",
                                    "everything in one fading group", "opaque, shadows on",
                                    "opaque, paper grain and vignette off", "30 x 60 code block" };
    if (!machine_quiet("after the run")) return 3;
    printf("200 strokes + 50 filled shapes, scenario %d: %s, %lld frames, up to %d triangles, up to %d layers/frame\n",
           scenario, names[scenario], (long long)(L + 1), max_tris, max_layers);
    printf("  tessellation   avg %.2f ms  worst %.2f ms\n", tess / (double)(L + 1), tess_worst);
    const int counted = (int)(L + 1) - WARMUP;
    double sorted[8192];
    memcpy(sorted, times + WARMUP, sizeof(double) * (size_t)counted);
    qsort(sorted, (size_t)counted, sizeof(double), cmp_double);
    int over12 = 0;
    for (int64_t f = WARMUP; f <= L; f++) over12 += times[f] > 12.0;
    printf("  full frame (frames %d..%lld): median %.2f  p99 %.2f  worst %.2f ms at frame %lld; %d frames over 12 ms\n",
           WARMUP, (long long)L, sorted[counted / 2], sorted[(counted * 99) / 100], worst, (long long)worst_frame, over12);
    // what the early frames did
    long early_faults = 0, late_faults = 0;
    int early_growth = 0, late_growth = 0;
    for (int64_t f = 0; f <= L; f++) {
        if (f < WARMUP) {
            early_faults += faults[f];
            early_growth += grew[f] != 0;
        } else {
            late_faults += faults[f];
            late_growth += grew[f] != 0;
        }
    }
    printf("  warm-up frames 0..%d: %.2f / %.2f / %.2f ms (frames 0, 1, 2), %ld page faults, %d frames with memory growth\n",
           WARMUP - 1, times[0], times[1], times[2], early_faults, early_growth);
    printf("  frames %d..%lld: %ld page faults total, %d frames with memory growth", WARMUP, (long long)L, late_faults, late_growth);
    for (int64_t f = WARMUP; f <= L; f++) {
        if (grew[f]) printf(" [frame %lld:%s%s%s%s%s]", (long long)f, grew[f] & 1 ? " verts" : "", grew[f] & 2 ? " cmds" : "",
                            grew[f] & 4 ? " layers" : "", grew[f] & 8 ? " vbuf" : "", grew[f] & 16 ? " pool" : "");
    }
    printf("\n");
    printf("  page faults, frames 0..5:");
    for (int f = 0; f < 6; f++) printf(" %ld", faults[f]);
    printf("\n  slowest 5 measured frames:");
    {
        int64_t idx[8192];
        for (int64_t f = WARMUP; f <= L; f++) idx[f - WARMUP] = f;
        for (int a = 0; a < 5; a++) {
            int best = a;
            for (int b = a + 1; b < counted; b++) if (times[idx[b]] > times[idx[best]]) best = b;
            const int64_t t = idx[a]; idx[a] = idx[best]; idx[best] = t;
            printf(" [frame %lld: %.2f ms, %ld faults%s]", (long long)idx[a], times[idx[a]], faults[idx[a]], grew[idx[a]] ? ", memory growth" : "");
        }
    }
    printf("\n  median page faults per measured frame: ");
    {
        long fs[8192];
        for (int i = 0; i < counted; i++) fs[i] = faults[WARMUP + i];
        for (int a = 0; a < counted; a++) for (int b = a + 1; b < counted; b++) if (fs[b] < fs[a]) { long t = fs[a]; fs[a] = fs[b]; fs[b] = t; }
        printf("%ld\n", fs[counted / 2]);
    }
    if (profile) {
        // per-phase medians over the measured frames, and the split of the frames
        // whose TOTAL is the median and the p99
        printf("  profile (ms; frames %d..%lld):\n", WARMUP, (long long)L);
        static int64_t order[8192];
        for (int i = 0; i < counted; i++) order[i] = WARMUP + i;
        for (int a = 1; a < counted; a++) { // insertion sort by total time
            const int64_t v = order[a];
            int b = a - 1;
            while (b >= 0 && times[order[b]] > times[v]) { order[b + 1] = order[b]; b--; }
            order[b + 1] = v;
        }
        const int64_t fmed = order[counted / 2], fp99 = order[(counted * 99) / 100];
        printf("    %-28s %9s %9s %9s %12s %12s\n", "phase", "median", "p99", "max", "median frm", "p99 frm");
        double sum_med = 0, sum_f = 0, sum_p = 0;
        for (int k = 0; k < PH_COUNT; k++) {
            static double v[8192];
            for (int i = 0; i < counted; i++) v[i] = phases[WARMUP + i][k];
            qsort(v, (size_t)counted, sizeof(double), cmp_double);
            printf("    %-28s %9.3f %9.3f %9.3f %12.3f %12.3f\n", ph_names[k], v[counted / 2], v[(counted * 99) / 100],
                   v[counted - 1], phases[fmed][k], phases[fp99][k]);
            sum_med += v[counted / 2];
            sum_f += phases[fmed][k];
            sum_p += phases[fp99][k];
        }
        printf("    %-28s %9.3f %9s %9s %12.3f %12.3f\n", "sum of phases", sum_med, "", "", sum_f, sum_p);
        printf("    %-28s %9s %9s %9s %12.3f %12.3f   (frames %lld and %lld)\n", "measured frame time", "", "", "",
               times[fmed], times[fp99], (long long)fmed, (long long)fp99);
    }
    ani_mesh_free(&m);
    ani_renderer_shutdown(&r);
    ani_offscreen_destroy(&off);
    ani_gfx_shutdown();
    an_scene_destroy(s);
    return 0;
}
