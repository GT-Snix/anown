#ifndef SPIKE_EXPORT_MTL_H
#define SPIKE_EXPORT_MTL_H

#include "sokol_gfx.h"

const void *spike_mtl_device(void);
void *spike_pool_push(void);
void spike_pool_pop(void *pool);
// Blocking copy of an RGBA8 image into dst (width*height*4 bytes, rows top-down).
void spike_readback_rgba8(sg_image img, int width, int height, void *dst);

#endif
