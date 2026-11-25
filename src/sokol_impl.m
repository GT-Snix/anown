// The one translation unit that holds the sokol implementations.
// Must be Objective-C on macOS (see sokol_app.h / sokol_gfx.h).
#define SOKOL_IMPL
#ifndef SOKOL_METAL
#define SOKOL_METAL
#endif
// anown is a library: the user's program owns main(), and an_window_run()
// calls sapp_run(). Without this, sokol_app defines main() and expects sokol_main().
#define SOKOL_NO_ENTRY
#include "sokol_log.h"
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"

// Profiling only (examples/spike_frame_bench): a retained reference to the
// command buffer sokol is recording this frame's passes into, or NULL. Call
// before sg_commit(); read its GPU times with ani_mtl_cmd_buffer_times()
// (src/offscreen.m), which also releases it. Reads sokol's private state, so a
// sokol upgrade may break it; nothing but the bench uses it.
const void *ani_mtl_retain_frame_cmd_buffer(void);
const void *ani_mtl_retain_frame_cmd_buffer(void) {
    return _sg.mtl.cmd_buffer ? CFBridgingRetain(_sg.mtl.cmd_buffer) : NULL;
}
