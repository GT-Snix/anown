// Internal: headless Metal setup, offscreen render targets, GPU->CPU readback.
// Not part of the public API. Implemented in src/offscreen.m.
#ifndef ANOWN_OFFSCREEN_INTERNAL_H
#define ANOWN_OFFSCREEN_INTERNAL_H

#include "anown.h"
#include "sokol_gfx.h"

#include <stdbool.h>
#include <stddef.h>

// Set up sokol_gfx without a window, on the system default MTLDevice.
// Defaults: RGBA8 color, no depth, 1 sample. Returns false on failure.
bool ani_gfx_setup_headless(void);
void ani_gfx_shutdown(void);

// Autorelease pool around a frame. A headless program has no AppKit run loop
// draining pools, so wrap each frame (and setup) in push/pop.
void *ani_pool_push(void);
void ani_pool_pop(void *pool);

typedef struct ani_offscreen {
    int width, height, sample_count;
    sg_image image;        // RGBA8 color attachment (multisampled if sample_count > 1)
    sg_view color_view;
    sg_image resolve;      // sample_count > 1 only: single-sampled resolve target
    sg_view resolve_view;
    sg_image ds;           // optional depth-stencil (same sample count), for stencil masking
    sg_view ds_view;
    void *staging;         // retained id<MTLBuffer>, shared storage
    // the last readback, for profiling (seconds, mach absolute time, the same
    // clock as CLOCK_UPTIME_RAW): the blit's GPU start and end, when the wait
    // returned, when the copy to dst finished
    double rb_gpu_start, rb_gpu_end, rb_waited, rb_copied;
} ani_offscreen;

// sample_count 0 or 1: no MSAA. With MSAA, readback reads the resolved image.
bool ani_offscreen_make(ani_offscreen *off, int width, int height, int sample_count);
// Same, plus a depth-stencil attachment (SG_PIXELFORMAT_DEPTH_STENCIL), which the
// renderer needs for shadow masking. Pipelines drawing into it must declare that
// depth format.
bool ani_offscreen_make_ds(ani_offscreen *off, int width, int height, int sample_count);
void ani_offscreen_destroy(ani_offscreen *off);

// Begin a pass into the target, cleared to `clear`. End it with sg_end_pass().
void ani_offscreen_begin(const ani_offscreen *off, an_color clear);

// Bytes in one frame: width * height * 4.
size_t ani_offscreen_frame_bytes(const ani_offscreen *off);

// Copy the target into dst as tightly packed RGBA8, rows top-down.
// Call after sg_commit(). Blocks until the GPU has finished the frame.
void ani_offscreen_readback(ani_offscreen *off, void *dst);

// Profiling only: a command buffer from ani_mtl_retain_frame_cmd_buffer()
// (src/sokol_impl.m). Waits for it, writes its GPU start and end (seconds, mach
// absolute time) and releases it. False if cb is NULL.
const void *ani_mtl_retain_frame_cmd_buffer(void);
bool ani_mtl_cmd_buffer_times(const void *cb, double *gpu_start, double *gpu_end);

#endif
