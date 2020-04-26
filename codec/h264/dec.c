
#include "mpu.h"
#include "mbuf.h"
#include "media_buffer.h"
#include "my_errno.h"
#include "mem.h"
#include "h264.h"
#include "libavcodec/avcodec.h"

typedef struct {
    struct list_head entry;
    uint64_t pts;
    uint64_t seq;
} frame_time_t;

typedef struct {
    AVCodecContext* context;
    AVFrame* frame;
    video_format* in;
    video_format* out;
    media_iden_t id;
    uint32_t angle;
    int32_t hold;

    uint64_t last_pts, last_seq;
    struct list_head pts_free;
    struct list_head pts_used;
    frame_time_t times[8];

    uintptr_t bytes;
    uintptr_t nr, seq;
    uint8_t buffer[256 * 1024];
} ffmpeg_wrapper_t;

static void ffmpeg_frame_put(void* opaque, uint8_t* data)
{
    (void) data;
    struct my_buffer* mbuf = (struct my_buffer *) opaque;
    mbuf->mop->free(mbuf);
}

static int frame_buffer_get(struct AVCodecContext* s, AVFrame* frame, int flags)
{
    int line_width[4];
    int panel_height[4];

    int width = frame->width;
    int height = frame->height;
    avcodec_align_dimensions2(s, &width, &height, frame->linesize);
    ffmpeg_wrapper_t* codec = (ffmpeg_wrapper_t *) s->opaque;

    uintptr_t pixel_bytes;
    video_format* fmt = NULL;
    switch ((uintptr_t) s->pix_fmt) {
        case AV_PIX_FMT_BGRA:
            pixel_bytes = 4;
            fmt = video_raw_format(codec->in->pixel, csp_bgra);
            line_width[0] = width;
            panel_height[0] = height;
            break;
        case AV_PIX_FMT_RGB24:
            pixel_bytes = 3;
            fmt = video_raw_format(codec->in->pixel, csp_rgb);
            line_width[0] = width;
            panel_height[0] = height;
            break;

        // h264 encoded with x264 from bgra will get this
        case AV_PIX_FMT_GBRP:
            pixel_bytes = 1;
            fmt = video_raw_format(codec->in->pixel, csp_gbrp);
            line_width[0] = line_width[1] = line_width[2] = width;
            panel_height[0] = panel_height[1] = panel_height[2] = height;
            break;

        // h264 encoded with x264 from nv12 will get this
        case AV_PIX_FMT_YUVJ420P:
        case AV_PIX_FMT_YUV420P:
            pixel_bytes = 1;
            fmt = video_raw_format(codec->in->pixel, csp_i420);
            line_width[0] = width;
            panel_height[0] = height;
            // ffmpeg promise width is even
            line_width[1] = line_width[2] = width / 2;
            panel_height[1] = panel_height[2] = height / 2;
            frame->linesize[1] /= 2;
            frame->linesize[2] /= 2;
            break;
    }

    if (fmt != codec->out) {
        if (fmt == NULL) {
            logmsg("color space %d can't support format %s\n", s->pix_fmt, codec->out->name);
            return -1;
        }
        codec->out = fmt;
    }

    uintptr_t bytes = 0;
    for (uintptr_t i = 0; i < fmt->pixel->panels; ++i) {
        // ffmpeg promise linesize is 2 ^ x
        frame->linesize[i] -= 1;
        uintptr_t stride = pixel_bytes * line_width[i] + frame->linesize[i];
        frame->linesize[i] = stride & (~(frame->linesize[i]));
        bytes += frame->linesize[i] * panel_height[i];
    }

    // ffmpeg says some code may access extra 16 bytes
    // and cpu alignment should be honored.
    // arm64 require 4, however, some neon instruction like vst1
    // require alignment 32 at max.
    struct my_buffer* mbuf = mbuf_alloc_2((uint32_t) sizeof(media_buffer) + bytes + 32 + 16);
    if (mbuf == NULL) {
        return -1;
    }
    mbuf->ptr[1] += sizeof(media_buffer);
    mbuf->ptr[1] = (char *) roundup(((uintptr_t) mbuf->ptr[1]), 32);
    mbuf->length = bytes;

    media_buffer* media = (media_buffer *) mbuf->ptr[0];
    memset(media->vp, 0, sizeof(media->vp));
    media->pptr_cc = &codec->out->cc;
    media->iden = codec->id;
    media->frametype = 0;
    media->angle = codec->angle;
    media->fragment[0] = 0;
    media->fragment[1] = 1;

    frame->data[0] = (uint8_t *) mbuf->ptr[1];
    media->vp[0].ptr = mbuf->ptr[1];
    media->vp[0].stride = frame->linesize[0];
    media->vp[0].height = panel_height[0];
    for (uintptr_t i = 1; i < fmt->pixel->panels; ++i) {
        frame->data[i] = frame->data[i - 1] + frame->linesize[i - 1] * panel_height[i - 1];
        media->vp[i].ptr = (char *) frame->data[i];
        media->vp[i].stride = frame->linesize[i];
        media->vp[i].height = panel_height[i];
    }

    frame->extended_data = &frame->data[0];
    memset(frame->buf, 0, sizeof(frame->buf));
    // because documents says buf reference chunk address than data entry
    // e.g. ffmpeg don't know which data[x] buf referenced exactly.
    // ok, it is another word of ffmpeg will never try to get data[x] from
    // buf, then it should use buf only to call addref/release things.
    // in this condition, passing mbuf than data[x] to av_buffer_create is safe.
    frame->buf[0] = av_buffer_create((uint8_t *) mbuf->ptr[1], (int) mbuf->length, ffmpeg_frame_put, (uint8_t *) mbuf, 0);
    if (frame->buf[0] == NULL) {
        mbuf->mop->free(mbuf);
        return -1;
    }

    frame->extended_buf = NULL;
    frame->nb_extended_buf = 0;
    return 0;
}

static void* ffmpeg_open(fourcc** in, fourcc** out)
{
    ffmpeg_wrapper_t* wrapper = (ffmpeg_wrapper_t *) my_malloc(sizeof(ffmpeg_wrapper_t));
    if (wrapper == NULL) {
        return NULL;
    }
    wrapper->in = to_video_format(in);
    wrapper->out = to_video_format(out);
    wrapper->id = media_id_unkown;
    wrapper->angle = 0;
    wrapper->last_pts = wrapper->last_seq = 0;
    wrapper->bytes = 0;
    wrapper->nr = wrapper->seq = 0;
    wrapper->hold = 0;

    INIT_LIST_HEAD(&wrapper->pts_free);
    INIT_LIST_HEAD(&wrapper->pts_used);
    for (intptr_t i = 0; i < elements(wrapper->times); ++i) {
        list_add(&wrapper->times[i].entry, &wrapper->pts_free);
    }

    avcodec_register_all();
    AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (codec == NULL) {
        my_free(wrapper);
        return NULL;
    }
    my_assert(codec->capabilities & CODEC_CAP_DR1);

    AVCodecContext* context = avcodec_alloc_context3(codec);
    if (context == NULL) {
        my_free(wrapper);
        return NULL;
    }
    wrapper->context = context;

    AVFrame* frame_buffer = av_frame_alloc();
    if(frame_buffer == NULL){
        avcodec_free_context(&context);
        my_free(wrapper);
        return NULL;
    }
    wrapper->frame = frame_buffer;

    context->refcounted_frames = 1;
    context->opaque = (void *) wrapper;
    context->get_buffer2 = frame_buffer_get;
    //context->thread_type |= FF_THREAD_FRAME;
    //context->thread_type |= FF_THREAD_SLICE;
    context->flags |= CODEC_FLAG_LOW_DELAY | CODEC_FLAG_TRUNCATED;
    context->flags2 |= CODEC_FLAG2_CHUNKS;

    if (0 != avcodec_open2(context, codec, NULL)) {
        av_frame_free(&frame_buffer);
        avcodec_free_context(&context);
        my_free(wrapper);
        return NULL;
    }
    return wrapper;
}

static intptr_t vcl_frame(char* pch, uintptr_t bytes)
{
    char code[3] = {0, 0, 1};
    while (bytes > 0) {
        my_assert(pch[0] == 0 && pch[1] == 0);
        if (pch[2] == 0) {
            my_assert(pch[3] == 1);
            pch += 4;
            bytes -= 4;
        } else {
            my_assert(pch[2] == 1);
            pch += 3;
            bytes -= 3;
        }

        char* end = (char *) memmem(pch, bytes, code, 3);
        if (end == NULL) {
            end = pch + bytes;
        } else if (end[-1] == 0) {
            end -= 1;
        }

        intptr_t type = pch[0] & 0x1f;
        intptr_t length = (intptr_t) (end - pch);
        my_assert(bytes >= length);

        if (type >= 1 && type <= 5) {
            return 1;
        }

        pch = end;
        bytes -= length;
    }
    return 0;
}

static void frame_pts_push(ffmpeg_wrapper_t* wrapper, media_buffer* media)
{
    if (0 != (media->frametype & 0xf0)) {
        if (!vcl_frame(media->vp[0].ptr, media->vp[0].stride)) {
            return;
        }
    }

    struct list_head* ent = wrapper->pts_free.next;
    if (__builtin_expect(list_empty(ent), 0)) {
        ent = wrapper->pts_used.next;
    }
    list_del(ent);

    frame_time_t* frame = list_entry(ent, frame_time_t, entry);
    frame->pts = media->pts;
    frame->seq = media->seq;

    for (ent = wrapper->pts_used.prev; ent != &wrapper->pts_used; ent = ent->prev) {
        frame_time_t* ft = list_entry(ent, frame_time_t, entry);
        if (ft->pts < frame->pts) {
            break;
        }
    }
    list_add_tail(&frame->entry, ent);
    wrapper->hold += 1;
}

static void frame_pts_pop(ffmpeg_wrapper_t* wrapper, media_buffer* media)
{
    struct list_head* ent = wrapper->pts_used.next;
    frame_time_t* frame = list_entry(ent, frame_time_t, entry);
    media->pts = frame->pts;
    media->seq = frame->seq;

    list_del(ent);
    list_add(ent, &wrapper->pts_free);
    wrapper->hold -= 1;
    my_assert2(wrapper->hold >= 0, "ffmpeg input < output");
}

static int32_t ffmpeg_write(void* handle, struct my_buffer* mbuf, struct list_head* head, int32_t* delay)
{
    media_buffer* media = NULL;
    ffmpeg_wrapper_t* wrapper = (ffmpeg_wrapper_t *) handle;
    if (media_id_eqal(wrapper->id, media_id_unkown)) {
        if (mbuf == NULL) {
            return 0;
        }
        media = (media_buffer *) mbuf->ptr[0];
        wrapper->id = media->iden;
        my_assert(media->frametype == video_type_sps);
        my_assert2(media->fragment[1] == 1, "sps+pps frame must not being fragmented");
        stream_begin(mbuf);
    }

    // ffmpeg will never modify avpkt.
    AVPacket avpkt;
    avpkt.data = NULL;
    avpkt.size = 0;
    if (mbuf != NULL) {
        media = (media_buffer *) mbuf->ptr[0];
        if (media->fragment[0] == 0) {
            frame_pts_push(wrapper, media);
            wrapper->angle = media->angle;
        }

        if (media->fragment[1] == 1) {
            my_assert(wrapper->bytes == 0);
            av_init_packet(&avpkt);
            avpkt.data = (uint8_t *) media->vp[0].ptr;
            avpkt.size = (int) media->vp[0].stride;
        } else {
            memcpy(wrapper->buffer + wrapper->bytes, media->vp[0].ptr, (size_t) media->vp[0].stride);
            wrapper->bytes += mbuf->length;
            ++wrapper->nr;
            if (media->fragment[0] == media->fragment[1] - 1) {
                my_assert(wrapper->nr == media->fragment[1]);
                av_init_packet(&avpkt);
                avpkt.data = (uint8_t *) wrapper->buffer;
                avpkt.size = (int) wrapper->bytes;
                wrapper->bytes = 0;
                wrapper->nr = 0;
                wrapper->seq = 0;
            } else if (media->fragment[0] == 0) {
                wrapper->seq = (uintptr_t) media->seq;
            } else {
                my_assert(media->seq - media->fragment[0] == wrapper->seq);
            }
        }
    }

    int32_t nr = 0;
    while ((mbuf == NULL) || (avpkt.size > 0)) {
        int got = 0;
        int consumed = avcodec_decode_video2(wrapper->context, wrapper->frame, &got, &avpkt);
        if (consumed < 0) {
            consumed = -consumed;
            char* pch = (char *) &consumed;
            mark("video decode failed %d(%c%c%c%c)\n", consumed, pch[0], pch[1], pch[2], pch[3]);
            my_assert(got == 0);
            break;
        }

        if (got != 0) {
            AVBufferRef* bufref = wrapper->frame->buf[0];
            struct my_buffer* mbuf2 = (struct my_buffer *) av_buffer_get_opaque(bufref);
            // this works when ffmpeg won't try to reuse the buffer
            // instread of releasing the ref (if it does this, mbuf2 maybe writen unexpected).
            mbuf2 = mbuf2->mop->clone(mbuf2);
            av_frame_unref(wrapper->frame);

            if (mbuf2 != NULL) {
                media = (media_buffer *) mbuf2->ptr[0];
                if (list_empty(&wrapper->pts_used)) {
                    media->pts = wrapper->last_pts;
                    media->seq = wrapper->last_seq;
                    logmsg("should not reache here\n");
                    my_assert(wrapper->hold == 0);
                } else {
                    frame_pts_pop(wrapper, media);
                }
                list_add_tail(&mbuf2->head, head);
                ++nr;
            }
        } else if (consumed == 0) {
            my_assert(mbuf == NULL);
            break;
        }

        if (mbuf != NULL) {
            avpkt.size -= consumed;
            avpkt.data += consumed;
        } else {
            my_assert(consumed == 0);
        }
    }

    if (mbuf != NULL) {
        mbuf->mop->free(mbuf);
    }
    *delay = wrapper->hold;
    return nr;
}

static void ffmpeg_close(void* handle)
{
    ffmpeg_wrapper_t* wrapper = (ffmpeg_wrapper_t *) handle;
    stream_end(wrapper->id, &wrapper->in->cc);
    avcodec_close(wrapper->context);
    av_frame_free(&wrapper->frame);
    avcodec_free_context(&wrapper->context);
    my_free(wrapper);
}

static mpu_operation ffmpeg_ops = {
    ffmpeg_open,
    ffmpeg_write,
    ffmpeg_close
};

media_process_unit ffmpeg_h264_dec = {
    &h264_0x0.cc,
    &raw_0x0.cc,
    &ffmpeg_ops,
    10,
    mpu_decoder,
    "ffmpeg_h264_dec"
};
