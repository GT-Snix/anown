#include "anown.h"

#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"

typedef struct an_window_state {
    an_color clear;
} an_window_state;

static void on_init(void *user) {
    (void)user;
    sg_setup(&(sg_desc){
        .environment = sglue_environment(),
        .logger.func = slog_func,
    });
}

static void on_frame(void *user) {
    const an_window_state *st = user;
    sg_begin_pass(&(sg_pass){
        .action = {
            .colors[0] = {
                .load_action = SG_LOADACTION_CLEAR,
                .clear_value = { st->clear.r, st->clear.g, st->clear.b, st->clear.a },
            },
        },
        .swapchain = sglue_swapchain(),
    });
    sg_end_pass();
    sg_commit();
}

static void on_cleanup(void *user) {
    (void)user;
    sg_shutdown();
}

void an_window_run(const an_window_desc *desc) {
    static an_window_state st;
    an_window_desc d = desc ? *desc : (an_window_desc){ 0 };

    st.clear = d.clear.a > 0.0f ? d.clear : AN_CREAM;

    sapp_run(&(sapp_desc){
        .user_data = &st,
        .init_userdata_cb = on_init,
        .frame_userdata_cb = on_frame,
        .cleanup_userdata_cb = on_cleanup,
        .width = d.width > 0 ? d.width : 1280,
        .height = d.height > 0 ? d.height : 720,
        .window_title = d.title ? d.title : "anown",
        .high_dpi = true,
        .logger.func = slog_func,
    });
}
