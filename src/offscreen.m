// Headless Metal setup, offscreen render targets and GPU->CPU readback.
//
// sokol_gfx has no readback API (only buffer->buffer, buffer->image and
// image->image copies), so readback uses the native Metal handles sokol exposes:
// after sg_commit(), blit the target's MTLTexture into a shared MTLBuffer on
// sokol's own command queue and wait for it. This is ordered after the frame's
// render pass because sokol enqueues its command buffer when it acquires it
// (_sg_mtl_acquire_cmd_buffer). tests/test_readback.c guards this assumption.
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include "offscreen_internal.h"
#include "sokol_log.h"

#include <string.h>

extern void *objc_autoreleasePoolPush(void);
extern void objc_autoreleasePoolPop(void *pool);

static id<MTLDevice> g_device;

void *ani_pool_push(void) { return objc_autoreleasePoolPush(); }
void ani_pool_pop(void *pool) { objc_autoreleasePoolPop(pool); }

bool ani_gfx_setup_headless(void) {
    if (!g_device) g_device = MTLCreateSystemDefaultDevice();
    if (!g_device) return false;
    sg_setup(&(sg_desc){
        .environment = {
            .metal.device = (__bridge const void *)g_device,
            .defaults = {
                .color_format = SG_PIXELFORMAT_RGBA8,
                .depth_format = SG_PIXELFORMAT_NONE,
                .sample_count = 1,
            },
        },
        .logger.func = slog_func,
    });
    return sg_isvalid() && sg_query_backend() == SG_BACKEND_METAL_MACOS;
}

void ani_gfx_shutdown(void) {
    sg_shutdown();
}

bool ani_offscreen_make(ani_offscreen *off, int width, int height, int sample_count) {
    memset(off, 0, sizeof *off);
    if (width <= 0 || height <= 0 || !g_device) return false;
    const bool msaa = sample_count > 1;
    off->width = width;
    off->height = height;
    off->sample_count = msaa ? sample_count : 1;
    off->image = sg_make_image(&(sg_image_desc){
        // without MSAA, readback copies straight from this image
        .usage = { .color_attachment = true, .copy_src = !msaa },
        .width = width,
        .height = height,
        .pixel_format = SG_PIXELFORMAT_RGBA8,
        .sample_count = off->sample_count,
        .label = "anown-offscreen",
    });
    off->color_view = sg_make_view(&(sg_view_desc){ .color_attachment.image = off->image });
    bool ok = sg_query_image_state(off->image) == SG_RESOURCESTATE_VALID
           && sg_query_view_state(off->color_view) == SG_RESOURCESTATE_VALID;
    if (msaa) {
        off->resolve = sg_make_image(&(sg_image_desc){
            .usage = { .resolve_attachment = true, .copy_src = true },
            .width = width,
            .height = height,
            .pixel_format = SG_PIXELFORMAT_RGBA8,
            .sample_count = 1,
            .label = "anown-offscreen-resolve",
        });
        off->resolve_view = sg_make_view(&(sg_view_desc){ .resolve_attachment.image = off->resolve });
        ok = ok && sg_query_image_state(off->resolve) == SG_RESOURCESTATE_VALID
                && sg_query_view_state(off->resolve_view) == SG_RESOURCESTATE_VALID;
    }
    id<MTLBuffer> staging = [g_device newBufferWithLength:ani_offscreen_frame_bytes(off)
                                                  options:MTLResourceStorageModeShared];
    off->staging = (void *)CFBridgingRetain(staging);
    return ok && off->staging != NULL;
}

bool ani_offscreen_make_ds(ani_offscreen *off, int width, int height, int sample_count) {
    if (!ani_offscreen_make(off, width, height, sample_count)) return false;
    off->ds = sg_make_image(&(sg_image_desc){
        .usage.depth_stencil_attachment = true,
        .width = width,
        .height = height,
        .pixel_format = SG_PIXELFORMAT_DEPTH_STENCIL,
        .sample_count = off->sample_count,
        .label = "anown-offscreen-ds",
    });
    off->ds_view = sg_make_view(&(sg_view_desc){ .depth_stencil_attachment.image = off->ds });
    return sg_query_view_state(off->ds_view) == SG_RESOURCESTATE_VALID;
}

void ani_offscreen_destroy(ani_offscreen *off) {
    sg_destroy_view(off->ds_view);
    sg_destroy_image(off->ds);
    sg_destroy_view(off->resolve_view);
    sg_destroy_image(off->resolve);
    sg_destroy_view(off->color_view);
    sg_destroy_image(off->image);
    if (off->staging) CFRelease(off->staging);
    memset(off, 0, sizeof *off);
}

void ani_offscreen_begin(const ani_offscreen *off, an_color clear) {
    const bool msaa = off->sample_count > 1;
    sg_begin_pass(&(sg_pass){
        .action.colors[0] = {
            .load_action = SG_LOADACTION_CLEAR,
            // the MSAA surface is only needed until the resolve
            .store_action = msaa ? SG_STOREACTION_DONTCARE : SG_STOREACTION_STORE,
            .clear_value = { clear.r, clear.g, clear.b, clear.a },
        },
        .action.depth = { .load_action = SG_LOADACTION_CLEAR, .store_action = SG_STOREACTION_DONTCARE, .clear_value = 1 },
        .action.stencil = { .load_action = SG_LOADACTION_CLEAR, .store_action = SG_STOREACTION_DONTCARE, .clear_value = 0 },
        .attachments = {
            .colors[0] = off->color_view,
            .resolves[0] = off->resolve_view,   // invalid (id 0) without MSAA
            .depth_stencil = off->ds_view,      // invalid (id 0) unless made with _ds
        },
    });
}

size_t ani_offscreen_frame_bytes(const ani_offscreen *off) {
    return (size_t)off->width * (size_t)off->height * 4;
}

void ani_offscreen_readback(ani_offscreen *off, void *dst) {
    const NSUInteger row = (NSUInteger)off->width * 4;
    const NSUInteger size = row * (NSUInteger)off->height;
    id<MTLBuffer> staging = (__bridge id<MTLBuffer>)off->staging;

    sg_mtl_image_info info = sg_mtl_query_image_info(off->sample_count > 1 ? off->resolve : off->image);
    id<MTLTexture> tex = (__bridge id<MTLTexture>)info.tex[info.active_slot];
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)sg_mtl_command_queue();

    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:tex
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake((NSUInteger)off->width, (NSUInteger)off->height, 1)
                 toBuffer:staging
        destinationOffset:0
   destinationBytesPerRow:row
 destinationBytesPerImage:size];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    off->rb_waited = CACurrentMediaTime();
    off->rb_gpu_start = cb.GPUStartTime;
    off->rb_gpu_end = cb.GPUEndTime;

    memcpy(dst, staging.contents, size);
    off->rb_copied = CACurrentMediaTime();
}

bool ani_mtl_cmd_buffer_times(const void *cbp, double *gpu_start, double *gpu_end) {
    if (!cbp) return false;
    id<MTLCommandBuffer> cb = CFBridgingRelease(cbp);
    [cb waitUntilCompleted];
    *gpu_start = cb.GPUStartTime;
    *gpu_end = cb.GPUEndTime;
    return true;
}
