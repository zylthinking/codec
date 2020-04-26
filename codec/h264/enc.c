
#include "mpu.h"
#include "mbuf.h"
#include "media_buffer.h"
#include "my_errno.h"
#include "mem.h"
#include "h264.h"
#include "fmt.h"

typedef struct {
    x264_t* x264;
    struct my_buffer* sps_pps;
    uint32_t seq;
    media_iden_t id;
    fourcc** cc_ptr;
    uintptr_t reflost;
} x264_wrapper_t;

struct my_buffer* video_stream_init(x264_t* x264)
{
    x264_nal_t* sps_pps;
    int nr_nal;
    int n = x264_encoder_headers(x264, &sps_pps, &nr_nal);
    if (n == -1) {
        return NULL;
    }
    // x264_encoder_headers encode sps/pps/sei for each.
    my_assert(nr_nal == 3);

    struct my_buffer* mbuf = mbuf_alloc_2((uint32_t) (sizeof(media_buffer) + n));
    if (mbuf == NULL) {
        return NULL;
    }
    mbuf->ptr[1] = mbuf->ptr[0] + sizeof(media_buffer);
    mbuf->length -= sizeof(media_buffer);

    media_buffer* media = (media_buffer *) mbuf->ptr[0];
    memset(media->vp, 0, sizeof(media->vp));
    media->angle = 0;
    media->frametype = video_type_sps;
    media->fragment[0] = 0;
    media->fragment[1] = 1;
    media->vp[0].stride = (uint32_t) mbuf->length;
    media->vp[0].ptr = mbuf->ptr[1];

    char* pch = media->vp[0].ptr;
    uintptr_t bytes = media->vp[0].stride;
    for (int i = 0; i < nr_nal; ++i) {
        my_assert(bytes >= sps_pps[i].i_payload);
        bytes -= sps_pps[i].i_payload;

        if (NAL_SEI != sps_pps[i].i_type) {
            memcpy(pch, sps_pps[i].p_payload, sps_pps[i].i_payload);
            pch += sps_pps[i].i_payload;
        } else {
            mbuf->length -= sps_pps[i].i_payload;
        }
    }
    my_assert(bytes == 0);
    media->vp[0].stride = (uint32_t) mbuf->length;

    return mbuf;
}

static void* x264_open(fourcc** in, fourcc** out)
{
    x264_param_t param;
    intptr_t n = x264_param_default_preset(&param, "medium", "film");
    if (n == -1) {
        return NULL;
    }

    video_format* fmt_in = to_video_format(in);
    param.i_csp = x264_csp_from_fmt(fmt_in);
    param.i_width = fmt_in->pixel->size->width;
    param.i_height = fmt_in->pixel->size->height;
    param.i_bframe = 3;
    param.i_frame_reference = 1;
    param.i_bframe_pyramid = X264_B_PYRAMID_NORMAL;
    param.i_keyint_max = (int) fmt_in->caparam->gop;
    param.i_fps_num = (int) fmt_in->caparam->fps;
    param.i_fps_den = 1;
    param.i_timebase_num = 1;
    param.i_timebase_den = 1;
    param.i_threads = 2;
    param.b_vfr_input = 0;
    param.b_repeat_headers = 0;
    param.b_annexb = 1;
    param.b_stitchable = 1;

    param.rc.i_rc_method = X264_RC_ABR;
    param.rc.i_bitrate = (int) fmt_in->caparam->kbps;
    param.rc.i_vbv_max_bitrate = 0;
    param.rc.i_vbv_buffer_size = 0;
    param.rc.f_qcompress = 0;
    param.rc.i_lookahead = 0;
    param.rc.b_mb_tree = 0;

    param.analyse.i_me_method = X264_ME_HEX;
    param.analyse.i_subpel_refine = 5;
    param.analyse.i_weighted_pred = X264_WEIGHTP_SIMPLE;

    n = x264_param_apply_profile(&param, "main");
    if (n == -1) {
        return NULL;
    }

    x264_t* p = x264_encoder_open(&param);
    if (p == NULL) {
        return NULL;
    }

    x264_wrapper_t* codec = (x264_wrapper_t *) my_malloc(sizeof(x264_wrapper_t));
    if (codec == NULL) {
        x264_encoder_close(p);
        return NULL;
    }
    codec->seq = 0;
    codec->x264 = p;
    codec->cc_ptr = out;
    codec->reflost = 0;

    codec->sps_pps = video_stream_init(p);
    if (codec->sps_pps == NULL) {
        my_free(codec);
        x264_encoder_close(p);
        return NULL;
    }

    media_buffer* media = (media_buffer *) codec->sps_pps->ptr[0];
    media->pptr_cc = out;
    media->seq = codec->seq;
    media->pts = 0;
    media->iden = media_id_unkown;
    return codec;
}

static int32_t add_to_list(x264_wrapper_t* codec, x264_picture_t* pic_out, x264_nal_t* nal, uintptr_t nr, struct list_head* headp)
{
    int32_t n = 0;
    for (uintptr_t i = 0; i < nr; ++i) {
        int type = nal[i].i_type;
        // sei does not affect decoding
        if (type == NAL_SEI) {
            continue;
        }
        my_assert(type != NAL_SPS && type != NAL_PPS);

        type = pic_out->i_type;
        my_assert(X264_TYPE_KEYFRAME != type);
        my_assert(X264_TYPE_AUTO != type);

        codec->seq++;
        type = frame_type_from_x264(type);
        if (type != video_type_idr && type != video_type_i && codec->reflost == 1) {
            continue;
        }

        uintptr_t bytes = nal[i].i_payload;
        struct my_buffer* mbuf = (struct my_buffer *) mbuf_alloc_2((uint32_t) (bytes + sizeof(media_buffer)));
        if (mbuf == NULL) {
            if (reference_able(type)) {
                codec->reflost = 1;
            }
            continue;
        }

        mbuf->ptr[1] = mbuf->ptr[0] + sizeof(media_buffer);
        mbuf->length = bytes;
        memcpy(mbuf->ptr[1], nal[i].p_payload, bytes);

        media_buffer* stream = (media_buffer *) mbuf->ptr[0];
        memset(stream->vp, 0, sizeof(stream->vp));
        stream->frametype = type;
        stream->angle = (uint32_t) (intptr_t) pic_out->opaque;
        stream->fragment[0] = 0;
        stream->fragment[1] = 1;
        stream->iden = codec->id;
        stream->pptr_cc = codec->cc_ptr;
        stream->pts = pic_out->i_pts;
        stream->seq = codec->seq;
        stream->vp[0].stride = (uint32_t) mbuf->length;
        stream->vp[0].ptr = mbuf->ptr[1];

        if (reference_able(stream->frametype)) {
            if (stream->frametype == video_type_idr) {
                codec->reflost = 0;
            }
        }
        list_add_tail(&mbuf->head, headp);
        ++n;
    }
    return n;
}

static int32_t flush_delayed_frame(x264_wrapper_t* codec, struct list_head* head)
{
    x264_picture_t pic_out;
    x264_nal_t* nal;
    int nr_nal, n = 0;

    while (x264_encoder_delayed_frames(codec->x264)) {
        int bytes = x264_encoder_encode(codec->x264, &nal, &nr_nal, NULL, &pic_out);
        if (bytes < 0) {
            return n;
        }

        n += add_to_list(codec, &pic_out, nal, nr_nal, head);
    }
    return n;
}

static int32_t x264_write(void* handle, struct my_buffer* mbuf, struct list_head* head, int32_t* delay)
{
    (void) delay;
    x264_wrapper_t* codec = (x264_wrapper_t *) handle;
    if (__builtin_expect(mbuf == NULL, 0)) {
        if (0 && codec->sps_pps == NULL) {
            return flush_delayed_frame(codec, head);
        }
        return 0;
    }

    media_buffer* media = (media_buffer *) mbuf->ptr[0];
    video_format* fmt = to_video_format(media->pptr_cc);

    x264_picture_t pic;
    x264_picture_init(&pic);
    for (uint32_t i = 0; i < fmt->pixel->panels; ++i) {
        pic.img.plane[i] = (uint8_t *) media->vp[i].ptr;
        pic.img.i_stride[i] = (int) media->vp[i].stride;
    }
    pic.img.i_csp = x264_csp_from_fmt(fmt);
    pic.img.i_plane = fmt->pixel->panels;
    pic.i_pts = media->pts;
    pic.opaque = (void *) (intptr_t) media->angle;

    x264_picture_t pic_out;
    x264_nal_t* nal;
    int nr_nal;

    int bytes = x264_encoder_encode(codec->x264, &nal, &nr_nal, &pic, &pic_out);
    if(bytes < 0){
        return -1;
    }

    codec->id = media->iden;
    intptr_t n = add_to_list(codec, &pic_out, nal, nr_nal, head);
    if (codec->sps_pps) {
        media_buffer* media = (media_buffer *) codec->sps_pps->ptr[0];
        media->pts = pic.i_pts;
        media->iden = codec->id;
        media->angle = (uint32_t) (intptr_t) pic_out.opaque;
        list_add(&codec->sps_pps->head, head);
        stream_begin(codec->sps_pps);
        codec->sps_pps = NULL;
        n += 1;
    }
    mbuf->mop->free(mbuf);
    return (int32_t) n;
}

static void x264_close(void* handle)
{
    x264_wrapper_t* codec = (x264_wrapper_t *) handle;
    stream_end(codec->id, codec->cc_ptr);
    x264_encoder_close(codec->x264);
    if (codec->sps_pps != NULL) {
        codec->sps_pps->mop->free(codec->sps_pps);
    }
    my_free(codec);
}

static mpu_operation x264_ops = {
    x264_open,
    x264_write,
    x264_close
};

media_process_unit x264_h264_enc = {
    &raw_0x0.cc,
    &h264_0x0.cc,
    &x264_ops,
    10,
    mpu_encoder,
    "x264_h264_enc"
};
