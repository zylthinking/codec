
#ifndef h264_h
#define h264_h

#include "fmt.h"
#include "x264.h"

static __attribute__((unused)) int x264_csp_from_fmt(video_format* fmt)
{
    uint32_t csp = fmt->pixel->csp;
    switch (csp) {
        case csp_rgb: return X264_CSP_RGB;
        case csp_bgra: return X264_CSP_BGRA;

        case csp_nv12:
        case csp_nv12ref: return X264_CSP_NV12;

        case csp_nv21:
        case csp_nv21ref: return X264_CSP_NV21;

        case csp_i420: return X264_CSP_I420;
        case csp_i420ref: return X264_CSP_I420;
        default: my_assert(0);
    }
    return -1;
}

static __attribute__((unused)) int frame_type_from_x264(int type)
{
    switch (type) {
        case X264_TYPE_IDR: return video_type_idr;
        case X264_TYPE_I: return video_type_i;
        case X264_TYPE_P: return video_type_p;

        case X264_TYPE_B: return video_type_b;
        case X264_TYPE_BREF: return video_type_bref;

        case NAL_SEI: return video_type_sei;
        default: my_assert(0);
    }
    return -1;
}

#endif
