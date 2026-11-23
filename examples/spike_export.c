// spike_export: prove offscreen render -> CPU readback -> ffmpeg -> mp4.
//
// Headless: sokol_gfx is set up with our own MTLDevice, no sokol_app window.
// A cream 1920x1080 frame with a solid quad moving left to right; the quad's
// position is a function of the frame index only.
//
// usage: spike_export [out.mp4] [--frames N] [--no-encode]
//   --no-encode  skip ffmpeg, to measure render + readback alone
#include "sokol_gfx.h"
#include "sokol_log.h"
#include "spike_quad.glsl.h"
#include "spike_export_mtl.h"

#include "anown.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { W = 1920, H = 1080, FPS = 60 };

// Wall-clock is used ONLY to measure, never to drive the animation.
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
    const char *out_path = "out.mp4";
    int frames = 300;
    int encode = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-encode")) encode = 0;
        else out_path = argv[i];
    }
    if (frames < 2) frames = 2;

    void *pool = spike_pool_push();
    sg_setup(&(sg_desc){
        .environment = {
            .metal.device = spike_mtl_device(),
            .defaults = {
                .color_format = SG_PIXELFORMAT_RGBA8,
                .depth_format = SG_PIXELFORMAT_NONE,
                .sample_count = 1,
            },
        },
        .logger.func = slog_func,
    });
    if (sg_query_backend() != SG_BACKEND_METAL_MACOS) {
        fprintf(stderr, "expected Metal backend\n");
        return 1;
    }

    sg_image target = sg_make_image(&(sg_image_desc){
        .usage = { .color_attachment = true, .copy_src = true },
        .width = W,
        .height = H,
        .pixel_format = SG_PIXELFORMAT_RGBA8,
        .sample_count = 1,
        .label = "spike-target",
    });
    sg_view target_view = sg_make_view(&(sg_view_desc){
        .color_attachment.image = target,
    });
    sg_pipeline pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = sg_make_shader(spike_quad_shader_desc(sg_query_backend())),
        .colors[0].pixel_format = SG_PIXELFORMAT_RGBA8,
        .depth.pixel_format = SG_PIXELFORMAT_NONE,
        .sample_count = 1,
        .label = "spike-quad",
    });
    spike_pool_pop(pool);

    FILE *ff = NULL;
    if (encode) {
        char cmd[1024];
        // rgba (sRGB values) -> BT.709 limited-range yuv420p, tagged as such so
        // players don't guess BT.601 and shift the cream.
        snprintf(cmd, sizeof cmd,
                 "ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgba -s %dx%d -framerate %d -i - "
                 "-vf scale=out_color_matrix=bt709:out_range=tv "
                 "-c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p "
                 "-colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv "
                 "-movflags +faststart '%s'",
                 W, H, FPS, out_path);
        ff = popen(cmd, "w");
        if (!ff) { perror("popen ffmpeg"); return 1; }
    }

    const size_t frame_bytes = (size_t)W * H * 4;
    uint8_t *pixels = malloc(frame_bytes);
    if (!pixels) return 1;

    const an_color bg = AN_CREAM;
    const an_color ink = AN_HEX(0xFF48B0);
    const float qw = 240.0f, qh = 240.0f;

    double t_render = 0, t_readback = 0, t_write = 0, t_max_frame = 0;
    const double t0 = now_ms();

    for (int64_t frame = 0; frame < frames; frame++) {
        pool = spike_pool_push();
        const double a = now_ms();

        // position from the frame index only: linear sweep across the width
        const float u = (float)frame / (float)(frames - 1);
        const vs_params_t vsp = {
            .rect = { u * ((float)W - qw), ((float)H - qh) * 0.25f, qw, qh },
            .target = { (float)W, (float)H },
        };
        const fs_params_t fsp = { .color = { ink.r, ink.g, ink.b, ink.a } };

        sg_begin_pass(&(sg_pass){
            .action.colors[0] = {
                .load_action = SG_LOADACTION_CLEAR,
                .clear_value = { bg.r, bg.g, bg.b, bg.a },
            },
            .attachments.colors[0] = target_view,
        });
        sg_apply_pipeline(pip);
        sg_apply_uniforms(UB_vs_params, &SG_RANGE(vsp));
        sg_apply_uniforms(UB_fs_params, &SG_RANGE(fsp));
        sg_draw(0, 6, 1);
        sg_end_pass();
        sg_commit();
        const double b = now_ms();

        spike_readback_rgba8(target, W, H, pixels);
        const double c = now_ms();

        if (ff && fwrite(pixels, 1, frame_bytes, ff) != frame_bytes) {
            fprintf(stderr, "short write to ffmpeg at frame %lld\n", (long long)frame);
            return 1;
        }
        const double d = now_ms();

        t_render += b - a;
        t_readback += c - b;
        t_write += d - c;
        if (d - a > t_max_frame) t_max_frame = d - a;
        spike_pool_pop(pool);
    }

    const double t_loop = now_ms() - t0;
    const double e = now_ms();
    int rc = ff ? pclose(ff) : 0;
    const double t_flush = now_ms() - e;
    const double t_total = now_ms() - t0;

    printf("frames            %d (%dx%d RGBA8, %.1f MB/frame)\n", frames, W, H,
           (double)frame_bytes / 1e6);
    printf("render+commit     %.3f ms/frame\n", t_render / frames);
    printf("readback          %.3f ms/frame  (%.2f GB/s)\n", t_readback / frames,
           (double)frame_bytes * frames / (t_readback / 1e3) / 1e9);
    printf("write to ffmpeg   %.3f ms/frame  %s\n", t_write / frames,
           encode ? "(includes pipe backpressure from encoder)" : "(skipped)");
    printf("worst frame       %.3f ms\n", t_max_frame);
    printf("loop              %.1f ms  -> %.1f fps\n", t_loop, frames / (t_loop / 1e3));
    if (encode) {
        printf("ffmpeg flush      %.1f ms\n", t_flush);
        printf("end to end        %.1f ms  -> %.1f fps (%.2fx realtime at %d fps)\n", t_total,
               frames / (t_total / 1e3), (frames / (t_total / 1e3)) / FPS, FPS);
        printf("ffmpeg exit       %d, wrote %s\n", rc, out_path);
    }

    free(pixels);
    sg_shutdown();
    return rc != 0;
}
