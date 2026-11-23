// Metal glue for spike_export.c: headless device + GPU->CPU readback.
// sokol_gfx has no readback API (only buffer->buffer, buffer->image and
// image->image copies), so this uses the native handles sokol exposes.
#import <Metal/Metal.h>

#include "sokol_gfx.h"
#include "spike_export_mtl.h"

#include <string.h>

static id<MTLDevice> g_device;
static id<MTLBuffer> g_staging;

// libobjc's C entry points behind @autoreleasepool. A headless program has no
// NSApplication run loop draining pools, so each frame gets its own.
extern void *objc_autoreleasePoolPush(void);
extern void objc_autoreleasePoolPop(void *pool);

const void *spike_mtl_device(void) {
    if (!g_device) g_device = MTLCreateSystemDefaultDevice();
    return (__bridge const void *)g_device;
}

void *spike_pool_push(void) { return objc_autoreleasePoolPush(); }
void spike_pool_pop(void *pool) { objc_autoreleasePoolPop(pool); }

void spike_readback_rgba8(sg_image img, int width, int height, void *dst) {
    const NSUInteger row = (NSUInteger)width * 4;
    const NSUInteger size = row * (NSUInteger)height;
    if (!g_staging || g_staging.length < size) {
        g_staging = [g_device newBufferWithLength:size options:MTLResourceStorageModeShared];
    }

    sg_mtl_image_info info = sg_mtl_query_image_info(img);
    id<MTLTexture> tex = (__bridge id<MTLTexture>)info.tex[info.active_slot];
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)sg_mtl_command_queue();

    // sokol enqueues its frame command buffer when it acquires it, so this one
    // is ordered after the render pass submitted by sg_commit().
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:tex
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake((NSUInteger)width, (NSUInteger)height, 1)
                 toBuffer:g_staging
        destinationOffset:0
   destinationBytesPerRow:row
 destinationBytesPerImage:size];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];

    memcpy(dst, g_staging.contents, size);
}
