#ifndef MAGMA_GAME_FRAME_CAPTURE_H
#define MAGMA_GAME_FRAME_CAPTURE_H

#include "game/runtime.h"

typedef struct GmFrameCapture GmFrameCapture;

GmFrameCapture *gm_frame_capture_open(const GmConfig *cfg, char *err, int err_cap);
/* Call once per tick. Always advances hand-animation state; renders and
 * writes the tick-numbered PPM only when render is non-zero. */
int gm_frame_capture_write(GmFrameCapture *capture, GmRuntime *runtime,
                           const GmAction *action, int render,
                           char *err, int err_cap);
void gm_frame_capture_close(GmFrameCapture *capture);

/* The frame's 16x16 EntityRenderer.updateLightmap texels for world_time,
 * written into lut and returned. NULL (and lut untouched) outside the
 * overworld, where the folded per-dimension path applies instead. In worldmc
 * lightmap mode terrain vertices carry raw 0..15 sky/block levels, so every
 * terrain CrShadeCtx MUST bind this or core/shade.c multiplies the texel by
 * the level itself (0..15) and blows the face to black/white. */
const CrRgba *gm_lightmap_lut(CrRgba lut[256], const McSinTable *st,
                              long long world_time, int dimension);

#endif
