// an_scene_export: offscreen MSAA render -> readback -> ffmpeg (H.264 mp4).
#include "anown.h"
#include "offscreen_internal.h"
#include "render_internal.h"

#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPORT_SAMPLES 4

// Append `s` to `out` single-quoted for /bin/sh ('...' with ' -> '\'').
static bool shell_quote(char *out, size_t cap, const char *s) {
    size_t n = strlen(out);
    if (n + 1 >= cap) return false;
    out[n++] = '\'';
    for (; *s; s++) {
        const char *piece = (*s == '\'') ? "'\\''" : (char[2]){ *s, 0 };
        const size_t k = strlen(piece);
        if (n + k + 1 >= cap) return false;
        memcpy(out + n, piece, k);
        n += k;
    }
    if (n + 2 > cap) return false;
    out[n++] = '\'';
    out[n] = 0;
    return true;
}

int64_t an_scene_export(an_scene *s, const an_export_desc *desc) {
    if (!s) return -1;
    const an_export_desc d = desc ? *desc : (an_export_desc){ 0 };
    const char *path = d.path ? d.path : "out.mp4";
    const int64_t frames = d.frames > 0 ? d.frames : an_scene_length(s) + 1; // 0..L inclusive
    const bool master = d.quality == AN_EXPORT_MASTER;
    const int crf = d.crf > 0 ? d.crf : master ? AN_EXPORT_MASTER_CRF : AN_EXPORT_SHARE_CRF;

    // rgba (sRGB values) -> BT.709 limited-range YUV, tagged so players don't
    // assume BT.601 and shift colors; moov atom up front (+faststart) so it
    // streams. Share: 4:2:0 (plays everywhere). Master: 4:4:4, High 4:4:4
    // Predictive profile, full color resolution for re-editing.
    char cmd[4096];
    snprintf(cmd, sizeof cmd,
             "ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgba -s %dx%d -framerate %d -i - "
             // setparams marks the frames themselves: with ffmpeg 9 the output's
             // -color_primaries / -color_trc alone are overridden by the frames'
             // "unknown" and the file ends up untagged
             "-vf scale=out_color_matrix=bt709:out_range=tv,"
             "setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=tv "
             "-c:v libx264 -preset medium -crf %d %s "
             "-colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv "
             "-movflags +faststart ",
             s->width, s->height, s->fps, crf,
             master ? "-pix_fmt yuv444p -profile:v high444" : "-pix_fmt yuv420p -profile:v high");
    if (!shell_quote(cmd, sizeof cmd, path)) {
        fprintf(stderr, "anown: export path too long\n");
        return -1;
    }

    void *pool = ani_pool_push();
    ani_offscreen off = { 0 };
    ani_renderer r = { 0 };
    if (!ani_gfx_setup_headless()) {
        fprintf(stderr, "anown: headless Metal setup failed\n");
        ani_pool_pop(pool);
        return -1;
    }
    const bool gpu_ok = ani_offscreen_make_ds(&off, s->width, s->height, EXPORT_SAMPLES)
                     && ani_renderer_init(&r, SG_PIXELFORMAT_RGBA8, SG_PIXELFORMAT_DEPTH_STENCIL, EXPORT_SAMPLES);
    ani_pool_pop(pool);
    r.grain_auto_static = !master; // AN_GRAIN_AUTO: share exports get static grain
    unsigned char *pixels = gpu_ok ? malloc(ani_offscreen_frame_bytes(&off)) : NULL;
    if (!pixels) {
        fprintf(stderr, "anown: export setup failed\n");
        ani_gfx_shutdown();
        return -1;
    }

    // If ffmpeg is missing or dies, writing to the pipe raises SIGPIPE, which
    // would kill the process. Ignore it for the duration; fwrite reports it.
    struct sigaction ign = { .sa_handler = SIG_IGN }, old_pipe;
    sigaction(SIGPIPE, &ign, &old_pipe);

    bool ok = true;
    FILE *ff = popen(cmd, "w");
    if (!ff) {
        perror("anown: popen ffmpeg");
        ok = false;
    }

    const size_t frame_bytes = ani_offscreen_frame_bytes(&off);
    for (int64_t frame = 0; ok && frame < frames; frame++) {
        pool = ani_pool_push();
        ok = ani_renderer_prepare(&r, s, frame, s->width, s->height);
        ani_offscreen_begin(&off, s->background);
        if (ok) ani_renderer_draw(&r);
        sg_end_pass();
        sg_commit();
        if (ok) ani_offscreen_readback(&off, pixels);
        ani_pool_pop(pool);
        if (ok && fwrite(pixels, 1, frame_bytes, ff) != frame_bytes) {
            fprintf(stderr, "anown: ffmpeg stopped accepting frames at frame %lld\n",
                    (long long)frame);
            ok = false;
        }
    }

    if (ff) {
        const int rc = pclose(ff);
        if (rc != 0) {
            const int code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
            if (code == 127) fprintf(stderr, "anown: ffmpeg not found on PATH\n");
            else fprintf(stderr, "anown: ffmpeg failed (exit %d)\n", code);
            ok = false;
        }
    }
    sigaction(SIGPIPE, &old_pipe, NULL);

    free(pixels);
    ani_renderer_shutdown(&r);
    ani_offscreen_destroy(&off);
    ani_gfx_shutdown();
    return ok ? frames : -1;
}

// ---- single frame to PNG ---------------------------------------------------------

#include "stb_image_write.h"

bool an_scene_save_png(an_scene *s, int64_t frame, const char *path) {
    if (!s || !path) return false;
    void *pool = ani_pool_push();
    if (!ani_gfx_setup_headless()) {
        fprintf(stderr, "anown: headless Metal setup failed\n");
        ani_pool_pop(pool);
        return false;
    }
    ani_offscreen off = { 0 };
    ani_renderer r = { 0 };
    bool ok = ani_offscreen_make_ds(&off, s->width, s->height, EXPORT_SAMPLES)
           && ani_renderer_init(&r, SG_PIXELFORMAT_RGBA8, SG_PIXELFORMAT_DEPTH_STENCIL, EXPORT_SAMPLES);
    unsigned char *pixels = ok ? malloc(ani_offscreen_frame_bytes(&off)) : NULL;
    if (pixels) {
        ok = ani_renderer_prepare(&r, s, frame < 0 ? 0 : frame, s->width, s->height);
        ani_offscreen_begin(&off, s->background);
        if (ok) ani_renderer_draw(&r);
        sg_end_pass();
        sg_commit();
        if (ok) ani_offscreen_readback(&off, pixels);
    } else {
        ok = false;
    }
    ani_pool_pop(pool);
    if (ok && !stbi_write_png(path, s->width, s->height, 4, pixels, s->width * 4)) {
        fprintf(stderr, "anown: could not write %s\n", path);
        ok = false;
    }
    free(pixels);
    ani_renderer_shutdown(&r);
    ani_offscreen_destroy(&off);
    ani_gfx_shutdown();
    return ok;
}

// ---- frame dumps ----------------------------------------------------------------------------

typedef struct png_job {
    char path[1024];
    unsigned char *pixels;
    int w, h;
    bool ok;
} png_job;

static void *png_worker(void *arg) {
    png_job *j = arg;
    j->ok = stbi_write_png(j->path, j->w, j->h, 4, j->pixels, j->w * 4) != 0;
    return NULL;
}

int64_t an_scene_save_frames(an_scene *s, const an_frames_desc *desc) {
    if (!s || !desc || !desc->dir) return -1;
    const an_frames_desc d = *desc;
    const int every = d.every > 0 ? d.every : 1;
    const int64_t first = d.first > 0 ? d.first : 0, last = d.last > 0 ? d.last : an_scene_length(s);
    const char *prefix = d.prefix ? d.prefix : "frame";
    enum { BATCH = 8 };
    void *pool = ani_pool_push();
    if (!ani_gfx_setup_headless()) {
        fprintf(stderr, "anown: headless Metal setup failed\n");
        ani_pool_pop(pool);
        return -1;
    }
    ani_offscreen off = { 0 };
    ani_renderer r = { 0 };
    const size_t bytes = (size_t)s->width * (size_t)s->height * 4;
    png_job *jobs = calloc(BATCH, sizeof *jobs);
    bool ok = jobs && ani_offscreen_make_ds(&off, s->width, s->height, EXPORT_SAMPLES) &&
              ani_renderer_init(&r, SG_PIXELFORMAT_RGBA8, SG_PIXELFORMAT_DEPTH_STENCIL, EXPORT_SAMPLES);
    for (int k = 0; ok && k < BATCH; k++) {
        jobs[k].pixels = malloc(bytes);
        ok = jobs[k].pixels != NULL;
    }
    ani_pool_pop(pool);
    int64_t written = 0;
    for (int64_t f = first; ok && f <= last;) {
        int n = 0;
        for (; n < BATCH && f <= last; n++, f += every) {
            pool = ani_pool_push();
            ok = ani_renderer_prepare(&r, s, f, s->width, s->height);
            ani_offscreen_begin(&off, s->background);
            if (ok) ani_renderer_draw(&r);
            sg_end_pass();
            sg_commit();
            if (ok) ani_offscreen_readback(&off, jobs[n].pixels);
            ani_pool_pop(pool);
            if (!ok) break;
            snprintf(jobs[n].path, sizeof jobs[n].path, "%s/%s_%04lld.png", d.dir, prefix, (long long)f);
            jobs[n].w = s->width;
            jobs[n].h = s->height;
        }
        pthread_t th[BATCH];
        for (int k = 0; k < n; k++) pthread_create(&th[k], NULL, png_worker, &jobs[k]);
        for (int k = 0; k < n; k++) {
            pthread_join(th[k], NULL);
            if (!jobs[k].ok) {
                fprintf(stderr, "anown: could not write %s\n", jobs[k].path);
                ok = false;
            } else {
                written++;
            }
        }
    }
    for (int k = 0; jobs && k < BATCH; k++) free(jobs[k].pixels);
    free(jobs);
    ani_renderer_shutdown(&r);
    ani_offscreen_destroy(&off);
    ani_gfx_shutdown();
    return ok ? written : -1;
}
