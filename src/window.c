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
    // Playback. The wall clock only picks WHICH frame to show; what a frame
    // looks like is a pure function of the timeline (docs/timeline.md).
    double play_seconds; // position in the loop
    bool paused;
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
    bool draw = false;
    if (g_win.scene) {
        const an_scene *s = g_win.scene;
        const double loop = (double)(an_scene_length(s) + 1) / (double)s->fps;
        if (!g_win.paused) g_win.play_seconds += sapp_frame_duration();
        if (g_win.play_seconds >= loop) g_win.play_seconds = 0.0;
        // small epsilon so stepping by 1/fps never lands a hair short of a frame
        const int64_t frame = (int64_t)(g_win.play_seconds * (double)s->fps + 1e-6);
        // layers render in their own passes, before the swapchain pass begins
        draw = ani_renderer_prepare(&g_win.renderer, s, frame, sapp_width(), sapp_height());
    }
    sg_begin_pass(&(sg_pass){
        .action.colors[0] = {
            .load_action = SG_LOADACTION_CLEAR,
            .clear_value = { c.r, c.g, c.b, c.a },
        },
        .swapchain = sglue_swapchain(),
    });
    if (draw) ani_renderer_draw(&g_win.renderer);
    sg_end_pass();
    sg_commit();
}

static void on_event(const sapp_event *e) {
    if (!g_win.scene || e->type != SAPP_EVENTTYPE_KEY_DOWN) return;
    const an_scene *s = g_win.scene;
    const double frame_s = 1.0 / (double)s->fps;
    const double last = (double)an_scene_length(s) * frame_s;
    switch (e->key_code) {
    case SAPP_KEYCODE_SPACE: g_win.paused = !g_win.paused; break;
    case SAPP_KEYCODE_RIGHT:
        g_win.paused = true;
        g_win.play_seconds += frame_s;
        if (g_win.play_seconds > last) g_win.play_seconds = last;
        break;
    case SAPP_KEYCODE_LEFT:
        g_win.paused = true;
        g_win.play_seconds -= frame_s;
        if (g_win.play_seconds < 0.0) g_win.play_seconds = 0.0;
        break;
    default: break;
    }
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
        .event_cb = on_event,
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
