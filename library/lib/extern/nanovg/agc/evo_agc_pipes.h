#ifndef BOREALIS_AGC_PIPES_H
#define BOREALIS_AGC_PIPES_H

#include "shaders/ui_backdrop_blur_pipe.h"
#include "shaders/ui_screen_2d_pipe.h"

/* Generated GPL-3.0 video artifacts are imported independently; each optional
 * pipeline still fails closed if shader creation or linking fails. */
#define EVO_AGC_HAVE_VIDEO_PIPES 1
#include "shaders/video_yuv_nv12_pipe.h"
#include "shaders/video_yuv_p010_hdr_pipe.h"
#endif
