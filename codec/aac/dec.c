
#include "mpu.h"
#include "mbuf.h"
#include "media_buffer.h"
#include "my_errno.h"
#include "mem.h"
#include "libavcodec/avcodec.h"

typedef struct {
    AVCodecContext* context;
    AVFrame* frame;
    audio_format* in;
    audio_format* out;

    intptr_t nb_frame;
    intptr_t bytes;
    intptr_t seq;
    char buffer[512];
} ffmpeg_wrapper_t;

static void ffmpeg_frame_put(void* opaque, uint8_t* data)
{
    (void) data;
    struct my_buffer* mbuf = (struct my_buffer *) opaque;
    mbuf->mop->free(mbuf);
}

static int frame_buffer_get(struct AVCodecContext* s, AVFrame* frame, int flags)
{
    my_assert(frame->format == AV_SAMPLE_FMT_FLTP);
    my_assert(frame->nb_extended_buf == 0);
    audio_format* fmt = (audio_format *) s->opaque;
    // the frame->channels changes in the first call and the next calls
    // set it with my correct value.
    frame->channels = fmt->pcm->channel;

    int channel_bytes = frame->nb_samples * sizeof(float);
    uintptr_t bytes = channel_bytes * frame->channels;
    frame->linesize[0] = (int) channel_bytes;

    // ffmpeg says some code may access extra 16 bytes
    // and cpu alignment should be honored.
    // arm64 require 4, however, some neon instruction like vst1
    // require alignment 32 at max.
    struct my_buffer* mbuf = mbuf_alloc_2((uint32_t) (sizeof(media_buffer) + bytes + 32 + 16));
    if (mbuf == NULL) {
        return -1;
    }
    mbuf->length = bytes;
    mbuf->ptr[1] += sizeof(media_buffer);
    mbuf->ptr[1] = (char *) roundup(((uintptr_t) mbuf->ptr[1]), 32);

    media_buffer* media = (media_buffer *) mbuf->ptr[0];
    memset(media->vp, 0, sizeof(media->vp));
    media->frametype = 0;
    media->angle = 0;
    media->fragment[0] = 0;
    media->fragment[1] = 1;

    media->vp[0].ptr = mbuf->ptr[1];
    media->vp[0].stride = channel_bytes;
    media->vp[0].height = 1;
    frame->data[0] = (uint8_t *) media->vp[0].ptr;
    if (frame->channels) {
        media->vp[1].ptr = mbuf->ptr[1] + channel_bytes;
        media->vp[1].stride = channel_bytes;
        media->vp[1].height = 1;
        frame->data[1] = (uint8_t *) media->vp[1].ptr;
    }
    frame->extended_data = frame->data;

    // because documents says buf reference chunk address than data entry
    // e.g. ffmpeg don't know which data[x] buf referenced exactly.
    // ok, it is another word of ffmpeg will never try to get data[x] from
    // buf, then it should use buf only to call addref/release things.
    // in this condition, passing mbuf than data[x] to av_buffer_create is safe.
    frame->buf[0] = av_buffer_create((uint8_t *) mbuf->ptr[1], (int) bytes, ffmpeg_frame_put, (uint8_t *) mbuf, 0);
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
    wrapper->bytes = 0;
    wrapper->nb_frame = 0;
    wrapper->seq = 0;
    wrapper->in = to_audio_format(in);
    wrapper->out = to_audio_format(out);

    avcodec_register_all();
    AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
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
    context->channels = wrapper->in->pcm->channel;
    context->sample_rate = wrapper->in->pcm->samrate;
    context->refcounted_frames = 1;
    context->opaque = wrapper->out;
    context->get_buffer2 = frame_buffer_get;

    context->flags |= CODEC_FLAG_LOW_DELAY;
    if (codec->capabilities & CODEC_CAP_TRUNCATED) {
        context->flags |= CODEC_FLAG_TRUNCATED;
        context->flags2 |= CODEC_FLAG2_CHUNKS;
    }

    if (0 != avcodec_open2(context, codec, NULL)) {
        av_frame_free(&frame_buffer);
        avcodec_free_context(&context);
        my_free(wrapper);
        return NULL;
    }
    return wrapper;
}

static uint64_t pts_calc(audio_format* fmt, int32_t bytes, uint64_t pts)
{
    fraction frac = fmt->ops->ms_from(fmt, bytes, 0);
    pts += frac.num / frac.den;
    return pts;
}

static void sample_format_convert(ffmpeg_wrapper_t* wrapper, struct my_buffer* mbuf)
{
    my_assert(wrapper->context->sample_fmt == AV_SAMPLE_FMT_FLTP);

    media_buffer* media = (media_buffer *) mbuf->ptr[0];
    float *buf1 = (float *) media->vp[0].ptr, *buf2 = (float *) media->vp[1].ptr;
    intptr_t samples = media->vp[0].stride / sizeof(float);
    mbuf->length = samples * wrapper->out->pcm->channel * wrapper->out->pcm->sambits / 8;
    media->vp[0].stride = (uint32_t) mbuf->length;
    media->vp[1].ptr = NULL;
    media->vp[1].stride = 0;
    media->vp[1].height = 0;

    intptr_t nr1 = (samples >> 2);
    intptr_t nr2 = (samples & 3);
    int16_t factor = (1 << 15);
    int val0, val1;
    int16_t* intp = (int16_t *) buf1;

#define convert_sample1() \
do { \
    val0 = (int) lrintf((*(buf1++) * factor)); \
    *(intp++) = clip_int16(val0); \
} while (0)

#define convert_sample2() \
do { \
    intp = (int16_t *) buf1; \
    val0 = (int) lrintf((*(buf1++) * factor)); \
    val1 = (int) lrintf((*(buf2++) * factor)); \
    intp[0] = clip_int16(val0); \
    intp[1] = clip_int16(val1); \
} while (0)

    if (wrapper->out->pcm->channel == 2) {
        for (int i = 0; i < nr1; ++i) {
            convert_sample2();
            convert_sample2();
            convert_sample2();
            convert_sample2();
        }

        for (int i = 0; i < nr2; ++i) {
            convert_sample2();
        }
    } else {
        for (int i = 0; i < nr1; ++i) {
            convert_sample1();
            convert_sample1();
            convert_sample1();
            convert_sample1();
        }

        for (int i = 0; i < nr2; ++i) {
            convert_sample1();
        }
    }
#undef convert_sample1
#undef convert_sample2
}

static struct my_buffer* pcm_buffer(ffmpeg_wrapper_t* wrapper)
{
    struct my_buffer* mbuf = NULL;
    media_buffer* media = NULL;
    AVFrame* frame = wrapper->frame;

    if (wrapper->context->get_buffer2 == frame_buffer_get) {
        AVBufferRef* bufref = wrapper->frame->buf[0];
        mbuf = (struct my_buffer *) av_buffer_get_opaque(bufref);
        mbuf = mbuf->mop->clone(mbuf);
        media = (media_buffer *) mbuf->ptr[0];
        media->vp[0].stride = frame->nb_samples * sizeof(float);
        media->vp[0].height = 1;
        if (media->vp[1].ptr != NULL) {
            media->vp[1].stride = media->vp[0].stride;
            media->vp[1].height = 1;
        }
        av_frame_unref(wrapper->frame);
        return mbuf;
    }

    int channel_bytes = frame->nb_samples * sizeof(float);
    int bytes = channel_bytes * frame->channels;
    my_assert(frame->format == AV_SAMPLE_FMT_FLTP);
    mbuf = mbuf_alloc_2(sizeof(media_buffer) + bytes);
    if (mbuf == NULL) {
        return NULL;
    }
    mbuf->ptr[1] += sizeof(media_buffer);
    mbuf->length -= sizeof(media_buffer);

    media = (media_buffer *) mbuf->ptr[0];
    memset(media->vp, 0, sizeof(media->vp));
    media->angle = 0;
    media->fragment[0] = 0;
    media->fragment[1] = 1;

    int i;
    char* pch = mbuf->ptr[1];
    uint8_t* pcm = frame->extended_data[0];
    for (i = 0; i < frame->channels; ++i) {
        my_assert(pcm != NULL);
        memcpy(pch, pcm, channel_bytes);
        media->vp[i].ptr = pch;
        media->vp[i].stride = channel_bytes;
        media->vp[i].height = 1;

        pch += channel_bytes;
        pcm = frame->extended_data[i + 1];
    }
    my_assert(frame->extended_data[i] == NULL);

    return mbuf;
}

static int32_t ffmpeg_write(void* handle, struct my_buffer* mbuf, struct list_head* head, int32_t* delay)
{
    (void) delay;
    if (__builtin_expect(mbuf == NULL, 0)) {
        return 0;
    }

    AVPacket avpkt;
    av_init_packet(&avpkt);
    ffmpeg_wrapper_t* wrapper = (ffmpeg_wrapper_t *) handle;
    media_buffer* media1 = (media_buffer *) mbuf->ptr[0];
    int32_t bytes = 0;

    if (wrapper->bytes > 0) {
        intptr_t nb = my_min(mbuf->length, wrapper->nb_frame);
        memcpy(&wrapper->buffer[wrapper->bytes], mbuf->ptr[1], nb);
        mbuf->ptr[1] += nb;
        mbuf->length -= nb;
        avpkt.data = (uint8_t *) wrapper->buffer;
        avpkt.size = (int) (wrapper->bytes + nb);
    } else {
        avpkt.data = (uint8_t *) mbuf->ptr[1];
        avpkt.size = (int) mbuf->length;
    }

    while (avpkt.size > 0) {
        int got = 0;
        int consumed = avcodec_decode_audio4(wrapper->context, wrapper->frame, &got, &avpkt);
        if (__builtin_expect(consumed < 0, 0)) {
            logmsg("avcodec_decode_audio4 failed %d\n", consumed);
            my_assert(got == 0);
            wrapper->bytes = 0;
            break;
        }

        if (consumed == 0) {
            if (avpkt.data == (uint8_t *) wrapper->buffer && mbuf->length > 0) {
                intptr_t nb = my_min(mbuf->length, sizeof(wrapper->buffer) - avpkt.size);
                my_assert(nb > 0);
                memcpy(&wrapper->buffer[avpkt.size], mbuf->ptr[1], nb);
                mbuf->ptr[1] += nb;
                mbuf->length -= nb;
                avpkt.size += nb;
                continue;
            }

            my_assert(avpkt.size < sizeof(wrapper->buffer));
            if (avpkt.size > wrapper->nb_frame) {
                wrapper->nb_frame = avpkt.size;
            }

            if (wrapper->buffer != (char *) avpkt.data) {
                memcpy(wrapper->buffer, avpkt.data, avpkt.size);
            }
            wrapper->bytes = avpkt.size;
            break;
        }

        my_assert(consumed > wrapper->bytes);
        if (consumed > wrapper->nb_frame) {
            wrapper->nb_frame = consumed;
        }

        if (avpkt.data == (uint8_t *) wrapper->buffer) {
            int n = avpkt.size - consumed;
            mbuf->ptr[1] -= n;
            mbuf->length += n;
            avpkt.data = (uint8_t *) mbuf->ptr[1];
            avpkt.size = (int) mbuf->length;
        } else {
            avpkt.size -= consumed;
            avpkt.data += consumed;
        }

        if (__builtin_expect(got == 0, 0)) {
            continue;
        }

        struct my_buffer* mbuf2 = pcm_buffer(wrapper);
        if (__builtin_expect(mbuf2 != NULL, 1)) {
            media_buffer* media2 = (media_buffer *) mbuf2->ptr[0];
            media2->pptr_cc = &wrapper->out->cc;
            media2->iden = media1->iden;
            media2->seq = wrapper->seq++;
            media2->pts = (bytes == 0) ? media1->pts : pts_calc(wrapper->out, bytes, media1->pts);

            sample_format_convert(wrapper, mbuf2);
            list_add_tail(&mbuf2->head, head);
            bytes += mbuf2->length;
        }
    }

    mbuf->mop->free(mbuf);
    return bytes;
}

static void ffmpeg_close(void* handle)
{
    ffmpeg_wrapper_t* wrapper = (ffmpeg_wrapper_t *) handle;
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

media_process_unit aac_dec_8k16b1 = {
    &aac_8k16b1.cc,
    &pcm_8k16b1.cc,
    &ffmpeg_ops,
    10,
    mpu_decoder,
    "aac_dec_8k16b1"
};

media_process_unit aac_dec_8k16b2 = {
    &aac_8k16b2.cc,
    &pcm_8k16b2.cc,
    &ffmpeg_ops,
    10,
    mpu_decoder,
    "aac_dec_8k16b2"
};

media_process_unit aac_dec_16k16b1 = {
    &aac_16k16b1.cc,
    &pcm_16k16b1.cc,
    &ffmpeg_ops,
    10,
    mpu_decoder,
    "aac_dec_16k16b1"
};

media_process_unit aac_dec_16k16b2 = {
    &aac_16k16b2.cc,
    &pcm_16k16b2.cc,
    &ffmpeg_ops,
    10,
    mpu_decoder,
    "aac_dec_16k16b2"
};

media_process_unit aac_dec_22k16b1 = {
    &aac_22k16b1.cc,
    &pcm_22k16b1.cc,
    &ffmpeg_ops,
    10,
    mpu_decoder,
    "aac_dec_22k16b1"
};

media_process_unit aac_dec_22k16b2 = {
    &aac_22k16b2.cc,
    &pcm_22k16b2.cc,
    &ffmpeg_ops,
    10,
    mpu_decoder,
    "aac_dec_22k16b2"
};

media_process_unit aac_dec_24k16b1 = {
    &aac_24k16b1.cc,
    &pcm_24k16b1.cc,
    &ffmpeg_ops,
    10,
    mpu_decoder,
    "aac_dec_24k16b1"
};

media_process_unit aac_dec_24k16b2 = {
    &aac_24k16b2.cc,
    &pcm_24k16b2.cc,
    &ffmpeg_ops,
    10,
    mpu_decoder,
    "aac_dec_24k16b2"
};

media_process_unit aac_dec_32k16b1 = {
    &aac_32k16b1.cc,
    &pcm_32k16b1.cc,
    &ffmpeg_ops,
    10,
    mpu_decoder,
    "aac_dec_32k16b1"
};

media_process_unit aac_dec_32k16b2 = {
    &aac_32k16b2.cc,
    &pcm_32k16b2.cc,
    &ffmpeg_ops,
    10,
    mpu_decoder,
    "aac_dec_32k16b2"
};

media_process_unit aac_dec_44k16b1 = {
    &aac_44k16b1.cc,
    &pcm_44k16b1.cc,
    &ffmpeg_ops,
    10,
    mpu_decoder,
    "aac_dec_44k16b1"
};

media_process_unit aac_dec_44k16b2 = {
    &aac_44k16b2.cc,
    &pcm_44k16b2.cc,
    &ffmpeg_ops,
    10,
    mpu_decoder,
    "aac_dec_44k16b2"
};

media_process_unit aac_dec_48k16b1 = {
    &aac_48k16b1.cc,
    &pcm_48k16b1.cc,
    &ffmpeg_ops,
    10,
    mpu_decoder,
    "aac_dec_48k16b1"
};

media_process_unit aac_dec_48k16b2 = {
    &aac_48k16b2.cc,
    &pcm_48k16b2.cc,
    &ffmpeg_ops,
    10,
    mpu_decoder,
    "aac_dec_48k16b2"
};

media_process_unit aacplus_dec_8k16b1 = {
    &aacplus_8k16b1.cc,
    &pcm_16k16b2.cc,
    &ffmpeg_ops,
    10,
    mpu_decoder,
    "aacplus_dec_8k16b1"
};

media_process_unit aacplus_dec_8k16b2 = {
    &aacplus_8k16b2.cc,
    &pcm_16k16b2.cc,
    &ffmpeg_ops,
    10,
    mpu_decoder,
    "aac_dec_8k16b2"
};

media_process_unit aacplus_dec_11k16b1 = {
    &aacplus_11k16b1.cc,
    &pcm_22k16b2.cc,
    &ffmpeg_ops,
    10,
    mpu_decoder,
    "aacplus_dec_11k16b1"
};

media_process_unit aacplus_dec_11k16b2 = {
    &aacplus_11k16b2.cc,
    &pcm_22k16b2.cc,
    &ffmpeg_ops,
    10,
    mpu_decoder,
    "aacplus_dec_11k16b2"
};

media_process_unit aacplus_dec_16k16b1 = {
    &aacplus_16k16b1.cc,
    &pcm_32k16b2.cc,
    &ffmpeg_ops,
    10,
    mpu_decoder,
    "aacplus_dec_16k16b1"
};

media_process_unit aacplus_dec_16k16b2 = {
    &aacplus_16k16b2.cc,
    &pcm_32k16b2.cc,
    &ffmpeg_ops,
    10,
    mpu_decoder,
    "aacplus_dec_16k16b2"
};

media_process_unit aacplus_dec_22k16b1 = {
    &aacplus_22k16b1.cc,
    &pcm_44k16b2.cc,
    &ffmpeg_ops,
    10,
    mpu_decoder,
    "aac_dec_22k16b1"
};

media_process_unit aacplus_dec_22k16b2 = {
    &aacplus_22k16b2.cc,
    &pcm_44k16b2.cc,
    &ffmpeg_ops,
    10,
    mpu_decoder,
    "aacplus_dec_22k16b2"
};

media_process_unit aacplus_dec_24k16b1 = {
    &aacplus_24k16b1.cc,
    &pcm_48k16b2.cc,
    &ffmpeg_ops,
    10,
    mpu_decoder,
    "aacplus_dec_24k16b1"
};

media_process_unit aacplus_dec_24k16b2 = {
    &aacplus_24k16b2.cc,
    &pcm_48k16b2.cc,
    &ffmpeg_ops,
    10,
    mpu_decoder,
    "aacplus_dec_24k16b2"
};
