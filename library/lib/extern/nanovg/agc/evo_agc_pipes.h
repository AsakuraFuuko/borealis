#ifndef BOREALIS_AGC_PIPES_H
#define BOREALIS_AGC_PIPES_H

#include "shaders/ui_backdrop_blur_pipe.h"
#include "shaders/ui_screen_2d_pipe.h"

/* M1 only: the GPL-3.0 generated NV12 pipe is present locally. Keep the
 * broader video-pipe gate explicit so missing HDR/planar/upscaler artifacts
 * cannot accidentally become link failures. */
#define EVO_AGC_HAVE_VIDEO_PIPES 1
#include "shaders/video_yuv_nv12_pipe.h"
#endif
