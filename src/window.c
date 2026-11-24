// Live windows: an_window_run (plain clear) and an_scene_preview (a scene).
#include "anown.h"
#include "render_internal.h"

#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"

#include <stdio.h>

// Area outside the letterboxed scene in the preview.
#define LETTERBOX AN_HEX(0x1E1E1E)
#define PREVIEW_SAMPLES 4

typedef struct window_state {
    an_color clear;
    an_scene *scene; // NULL: plain clear window
    ani_renderer renderer;
    int64_t frame; // advances once per displayed frame; not used for timing yet
} window_state;

static window_state g_win;

static void on_init(void) {
    sg_setup(&(sg_desc){
        .environment = sglue_environment(),
        .logger.func = slog_func,
    });
    if (g_win.scene) {
        const sg_environment env = sglue_environment();
        if (!ani_renderer_init(&g_win.renderer, env.defaults.color_format,
                               env.defaults.depth_format, sapp_sample_count())) {
            fprintf(stderr, "anown: renderer setup failed\n");
            sapp_quit();
        }
    }
}

static void on_frame(void) {
    const an_color c = g_win.scene ? LETTERBOX : g_win.clear;
    sg_begin_pass(&(sg_pass){
        .action.colors[0] = {
            .load_action = SG_LOADACTION_CLEAR,
            .clear_value = { c.r, c.g, c.b, c.a },
        },
        .swapchain = sglue_swapchain(),
    });
    if (g_win.scene) ani_renderer_draw(&g_win.renderer, g_win.scene, sapp_width(), sapp_height());
    sg_end_pass();
    sg_commit();
    g_win.frame++;
}

static void on_cleanup(void) {
    if (g_win.scene) ani_renderer_shutdown(&g_win.renderer);
    sg_shutdown();
}

static void run(const char *title, int width, int height, int samples) {
    sapp_run(&(sapp_desc){
        .init_cb = on_init,
        .frame_cb = on_frame,
        .cleanup_cb = on_cleanup,
        .width = width,
        .height = height,
        .sample_count = samples,
        .window_title = title,
        .high_dpi = true,
        .logger.func = slog_func,
    });
}

void an_window_run(const an_window_desc *desc) {
    const an_window_desc d = desc ? *desc : (an_window_desc){ 0 };
    g_win = (window_state){ .clear = d.clear.a > 0.0f ? d.clear : AN_CREAM };
    run(d.title ? d.title : "anown", d.width > 0 ? d.width : 1280, d.height > 0 ? d.height : 720, 1);
}

void an_scene_preview(an_scene *s) {
    if (!s) return;
    g_win = (window_state){ .scene = s };
    // Window in points; with high_dpi the framebuffer is 2x on Retina, so a
    // 1920x1080 scene previews at full resolution in a 960x540 window.
    const int w = s->width, h = s->height;
    const int div = w > 1280 ? 2 : 1;
    run("anown preview", w / div, h / div, PREVIEW_SAMPLES);
}
