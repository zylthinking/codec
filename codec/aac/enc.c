
#include "mpu.h"
#include "mbuf.h"
#include "media_buffer.h"
#include "my_errno.h"
#include "mem.h"
#include "libavcodec/avcodec.h"

typedef struct {
    audio_format* fmt;
    fourcc** out;
    AVCodecContext* context;
    AVFrame* frame;
    media_iden_t iden;
    struct my_buffer* header;
    intptr_t nb[2];
    intptr_t nbuf;
    intptr_t hbuf;
    intptr_t seq;
} ffmpeg_wrapper_t;

static void ffmpeg_frame_put(void* opaque, uint8_t* data)
{
    (void) data;
    av_free(opaque);
}

static void mbuf_init(ffmpeg_wrapper_t* wrapper, struct my_buffer* mbuf)
{
    mbuf->ptr[1] += sizeof(media_buffer);
    mbuf->length -= sizeof(media_buffer);
    media_buffer* media = (media_buffer *) mbuf->ptr[0];
    memset(media->vp, 0, sizeof(media->vp));
    media->vp[0].ptr = mbuf->ptr[1];
    media->vp[0].stride = (uint32_t) mbuf->length;
    media->vp[0].height = 1;
    media->pptr_cc = wrapper->out;
    media->iden = wrapper->iden;
    media->fragment[0] = 0;
    media->fragment[1] = 1;
    media->frametype = 0;
    media->angle = 0;
    media->seq = 0;
    media->pts = 0;
}

static void* ffmpeg_open(fourcc** in, fourcc** out)
{
    avcodec_register_all();
    audio_format* fmt_in = to_audio_format(in);
    audio_format* fmt_out = to_audio_format(out);

    errno = ENOMEM;
    ffmpeg_wrapper_t* wrapper = heap_alloc(wrapper);
    if (wrapper == NULL) {
        return NULL;
    }
    wrapper->seq = 0;
    wrapper->fmt = fmt_in;
    wrapper->out = out;
    wrapper->iden = media_id_unkown;

    AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    my_assert(codec != NULL);
    my_assert(codec->capabilities & AV_CODEC_CAP_DELAY);

    wrapper->context = avcodec_alloc_context3(codec);
    if (wrapper->context == NULL) {
        my_free(wrapper);
        return NULL;
    }

    if (fmt_out->cc->code == codec_aacplus) {
        wrapper->context->profile = FF_PROFILE_AAC_HE;
    } else {
        wrapper->context->profile = FF_PROFILE_AAC_LOW;
    }

    wrapper->frame = av_frame_alloc();
    if(wrapper->frame == NULL){
        avcodec_free_context(&wrapper->context);
        my_free(wrapper);
        return NULL;
    }

    wrapper->context->sample_fmt = AV_SAMPLE_FMT_FLTP;
    wrapper->context->sample_rate = fmt_in->pcm->samrate;
    wrapper->context->channels = fmt_in->pcm->channel;
    wrapper->context->channel_layout = (fmt_in->pcm->channel == 2) ?
                                        AV_CH_LAYOUT_STEREO : AV_CH_LAYOUT_MONO;
    wrapper->context->bit_rate = 10 * fmt_in->pcm->samrate * fmt_in->pcm->channel / 3;

    AVDictionary* opts = NULL;
    av_dict_set(&opts, "strict", "experimental", 0);
    //wrapper->context->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    if (0 != avcodec_open2(wrapper->context, codec, &opts)) {
        av_frame_free(&wrapper->frame);
        avcodec_free_context(&wrapper->context);
        my_free(wrapper);
        return NULL;
    }

    wrapper->header = mbuf_alloc_2(sizeof(media_buffer) + wrapper->context->extradata_size);
    if (wrapper->header == NULL) {
        av_frame_free(&wrapper->frame);
        avcodec_free_context(&wrapper->context);
        my_free(wrapper);
        return NULL;
    }
    mbuf_init(wrapper, wrapper->header);
    media_buffer* media = (media_buffer *) wrapper->header->ptr[0];
    memcpy(media->vp[0].ptr, wrapper->context->extradata, media->vp[0].stride);

    wrapper->frame->nb_samples = wrapper->context->frame_size;
    wrapper->frame->channels = wrapper->context->channels;
    wrapper->frame->channel_layout = wrapper->context->channel_layout;
    wrapper->frame->format = AV_SAMPLE_FMT_FLTP;
    wrapper->frame->sample_rate = fmt_in->pcm->samrate;

    size_t channel_bytes = wrapper->frame->nb_samples * sizeof(float);
    size_t bytes = channel_bytes * fmt_in->pcm->channel;
    uint8_t* buffer = av_malloc(bytes);
    if (buffer == NULL) {
        av_frame_free(&wrapper->frame);
        avcodec_free_context(&wrapper->context);
        wrapper->header->mop->free(wrapper->header);
        my_free(wrapper);
        return NULL;
    }

    wrapper->frame->buf[0] = av_buffer_create(buffer, (int) bytes, ffmpeg_frame_put, buffer, 0);
    if (wrapper->frame->buf[0] == NULL) {
        av_free(buffer);
        av_frame_free(&wrapper->frame);
        avcodec_free_context(&wrapper->context);
        wrapper->header->mop->free(wrapper->header);
        my_free(wrapper);
        return NULL;
    }
    wrapper->frame->extended_buf = NULL;
    wrapper->frame->nb_extended_buf = 0;

    for (int i = 0; i < fmt_in->pcm->channel; ++i) {
        wrapper->frame->linesize[i] = (int) channel_bytes;
        wrapper->frame->data[i] = buffer + i * channel_bytes;
    }
    wrapper->frame->extended_data = wrapper->frame->data;

    // aac compression ratio is about 1/16 to 1/20
    // 1/5 should be large enough
    wrapper->nbuf = sizeof(media_buffer) +
                    channel_bytes * fmt_in->pcm->channel / 5;
    wrapper->hbuf = mbuf_hget(wrapper->nbuf, 16, 1);
    wrapper->nb[0] = bytes / 2;
    wrapper->nb[1] = 0;
    return wrapper;
}

static uint64_t pts_calc(audio_format* fmt, int32_t bytes, uint64_t pts)
{
    fraction frac = fmt->ops->ms_from(fmt, bytes, 0);
    pts += frac.num / frac.den;
    return pts;
}

static struct my_buffer* aac_buffer_alloc(ffmpeg_wrapper_t* wrapper)
{
    if (__builtin_expect(wrapper->hbuf == -1, 0)) {
        wrapper->hbuf = mbuf_hget(wrapper->nbuf, 16, 1);
    }

    struct my_buffer* mbuf = NULL;
    if (__builtin_expect(wrapper->hbuf != -1, 1)) {
        mbuf = mbuf_alloc_1(wrapper->hbuf);
    } else {
        mbuf = mbuf_alloc_2(wrapper->nbuf);
    }

    if (mbuf != NULL) {
        mbuf_init(wrapper, mbuf);
    }
    return mbuf;
}

static void avpack_free(struct AVPacket* pkt)
{
    struct my_buffer* mbuf = (struct my_buffer *) pkt->priv;
    mbuf->mop->free(mbuf);
}

static inline void deinterleave(int16_t* ch, int bytes, int16_t* ch1, int16_t* ch2)
{
    for (int i = 0; i < bytes; i += 4) {
        *ch1++ = *ch++;
        *ch2++ = *ch++;
    }
}

static inline void int16_to_float(int16_t* int16p, intptr_t samples)
{
    const float factor = 1.0 / (1 << 15);
    float* f = ((float *) int16p) + samples;
    int16p += samples;

    do {
        *(--f) = *(--int16p) * factor;
    } while ((intptr_t) f != (intptr_t) int16p);
}

static void sample_format_convert(AVFrame* frame)
{
    int16_t* ch1 = (int16_t *) frame->data[0];
    int16_t* ch2 = (int16_t *) frame->data[1];

    int channel_bytes = frame->linesize[0];
    if (ch2 != NULL) {
        deinterleave(ch1, channel_bytes, ch1,  ch2);
        int16_to_float(ch2, channel_bytes / 4);
    }
    int16_to_float(ch1, channel_bytes / 4);
}

static int32_t aac_encode(ffmpeg_wrapper_t* wrapper, media_buffer* media, int32_t* offset, struct list_head* head)
{
    AVFrame* frame = wrapper->frame;
    if (media == NULL) {
        if (!media_id_eqal(wrapper->iden, media_id_unkown)) {
            frame = NULL;
            goto LABEL;
        }
        return 0;
    }

    if (wrapper->header != NULL) {
        wrapper->iden = media->iden;
        media_buffer* media2 = (media_buffer *) wrapper->header->ptr[0];
        media2->iden = wrapper->iden;
        media2->seq = wrapper->seq++;
        media2->pts = (uint64_t) media->pts;
        stream_begin(wrapper->header);
        wrapper->header = NULL;
    }

    intptr_t need = wrapper->nb[0] - wrapper->nb[1];
    intptr_t have = media->vp[0].stride - *offset;
    intptr_t feed = my_min(need, have);
    uint64_t pts = media->pts;

    uint8_t* pcm = (uint8_t *) media->vp[0].ptr + *offset;
    *offset += feed;
    need -= (intptr_t) feed;
    if (wrapper->nb[1] > 0) {
        memcpy(frame->data[0] + wrapper->nb[1], pcm, feed);
        wrapper->nb[1] += feed;
    } else if (0 && have >= need) {
        frame->data[0] = pcm;
        if (*offset > 0) {
            pts = pts_calc(wrapper->fmt, *offset, media->pts);
        }
        frame->pts = (int64_t) pts;
    } else {
        memcpy(frame->data[0], pcm, feed);
        wrapper->nb[1] += feed;

        if (*offset > 0) {
            pts = pts_calc(wrapper->fmt, *offset, media->pts);
        }
        frame->pts = (int64_t) pts;
    }

    if (need > 0) {
        return 0;
    }
LABEL:
    wrapper->nb[1] = 0;
    sample_format_convert(frame);

    struct my_buffer* mbuf = aac_buffer_alloc(wrapper);
    if (mbuf == NULL) {
        return 0;
    }
    media = (media_buffer *) mbuf->ptr[0];

    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = (uint8_t *) media->vp[0].ptr;
    pkt.size = media->vp[0].stride;
    pkt.destruct = avpack_free;
    pkt.priv = mbuf;

    int got = 0;
    int n = avcodec_encode_audio2(wrapper->context, &pkt, frame, &got);
    if (n < 0 || got == 0) {
        return 0;
    }
    pkt.destruct = NULL;

    mbuf->length = pkt.size;
    media->vp[0].stride = pkt.size;
    media->seq = wrapper->seq++;
    media->pts = (uint64_t) pkt.pts;
    av_free_packet(&pkt);

    list_add_tail(&mbuf->head, head);
    return media->vp[0].stride;
}

static int32_t ffmpeg_write(void* handle, struct my_buffer* mbuf, struct list_head* head, int32_t* delay)
{
    (void) delay;
    int32_t offset = 0, bytes = 0;

    ffmpeg_wrapper_t* wrapper = (ffmpeg_wrapper_t *) handle;
    if (mbuf == NULL) {
        bytes = aac_encode(wrapper, NULL, NULL, head);
        return bytes;
    }

    media_buffer* media = (media_buffer *) mbuf->ptr[0];
    while (offset < media->vp[0].stride) {
        bytes += aac_encode(wrapper, media, &offset, head);
    }
    mbuf->mop->free(mbuf);
    return bytes;
}

static void ffmpeg_close(void* handle)
{
    ffmpeg_wrapper_t* wrapper = (ffmpeg_wrapper_t *) handle;
    av_frame_free(&wrapper->frame);
    avcodec_free_context(&wrapper->context);
    if (wrapper->hbuf != -1) {
        mbuf_reap(wrapper->hbuf);
    }
    if (wrapper->header != NULL) {
        wrapper->header->mop->free(wrapper->header);
    } else {
        stream_end(wrapper->iden, wrapper->out);
    }
    my_free(wrapper);
}

static mpu_operation ffmpeg_ops = {
    ffmpeg_open,
    ffmpeg_write,
    ffmpeg_close
};

media_process_unit aac_enc_8k16b1 = {
    &pcm_8k16b1.cc,
    &aac_8k16b1.cc,
    &ffmpeg_ops,
    10,
    mpu_encoder,
    "aac_enc_8k16b1"
};

media_process_unit aac_enc_8k16b2 = {
    &pcm_8k16b2.cc,
    &aac_8k16b2.cc,
    &ffmpeg_ops,
    10,
    mpu_encoder,
    "aac_enc_8k16b2"
};

media_process_unit aac_enc_16k16b1 = {
    &pcm_16k16b1.cc,
    &aac_16k16b1.cc,
    &ffmpeg_ops,
    10,
    mpu_encoder,
    "aac_enc_16k16b1"
};

media_process_unit aac_enc_16k16b2 = {
    &pcm_16k16b2.cc,
    &aac_16k16b2.cc,
    &ffmpeg_ops,
    10,
    mpu_encoder,
    "aac_enc_16k16b2"
};
