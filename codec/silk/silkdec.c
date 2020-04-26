
#include "mpu.h"
#include "mbuf.h"
#include "media_buffer.h"
#include "my_errno.h"
#include "mem.h"
#include "silk.h"
#include "interface/SKP_Silk_SDK_API.h"

struct silk_codec {
    void* obj;
    SKP_SILK_SDK_DecControlStruct control;
    uint64_t seq[2], pts;

    audio_format* fmt_out;
    intptr_t mbuf_handle;
    uint32_t bytes;
};

static void* silk_open(fourcc** in, fourcc** out)
{
    SKP_int32 size;
    SKP_int n = SKP_Silk_SDK_Get_Decoder_Size(&size);
    if (n != 0) {
        errno = EFAULT;
        return NULL;
    }

    struct silk_codec* codec = (struct silk_codec *) my_malloc(size + sizeof(struct silk_codec));
    if (codec == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    codec->obj = (void *) (codec + 1);

    n = SKP_Silk_SDK_InitDecoder(codec->obj);
    if (n != 0) {
        my_free(codec);
        errno = EFAULT;
        return NULL;
    }

    codec->seq[0] = codec->seq[1] = codec->pts = 0;
    codec->fmt_out = to_audio_format(out);
    codec->control.API_sampleRate = codec->fmt_out->pcm->samrate;
    fraction frac = pcm_bytes_from_ms(codec->fmt_out->pcm, 1);
    codec->bytes = silk_mpf * frac.num / frac.den;
    codec->mbuf_handle = mbuf_hget(sizeof(media_buffer) + codec->bytes, 8, 1);
    return codec;
}

static struct my_buffer* pcm_buffer_alloc(struct my_buffer* mbuf,
                                          uint32_t ms, media_buffer* stream, struct silk_codec* codec)
{
    if (mbuf == NULL) {
        if (__builtin_expect(codec->mbuf_handle == -1, 0)) {
            uint32_t bytes = sizeof(media_buffer) + codec->bytes;
            codec->mbuf_handle = mbuf_hget(bytes, 8, 1);
            if (codec->mbuf_handle == -1) {
                mbuf = mbuf_alloc_2(bytes);
            }
        } else {
            mbuf = mbuf_alloc_1(codec->mbuf_handle);
        }

        if (mbuf == NULL) {
            return NULL;
        }
        mbuf->length = codec->bytes;
        mbuf->ptr[1] = mbuf->ptr[0] + sizeof(media_buffer);
    }

    media_buffer* media = (media_buffer *) mbuf->ptr[0];
    memset(media->vp, 0, sizeof(media->vp));
    media->frametype = 0;
    media->angle = 0;
    media->fragment[0] = 0;
    media->fragment[1] = 1;
    media->pptr_cc = &codec->fmt_out->cc;
    media->pts = stream->pts + ms;
    media->iden = stream->iden;
    media->seq = codec->seq[0]++;
    media->vp[0].ptr = mbuf->ptr[1];
    media->vp[0].stride = (uint32_t) mbuf->length;
    media->vp[0].height = 1;
    return mbuf;
}

static uint32_t silk_muted(struct my_buffer* mbuf, uint32_t bytes)
{
    uintptr_t nb = mbuf->length;
    mbuf->length = bytes;
    uint32_t muted = silk_frame_muted(mbuf);
    mbuf->length = nb;
    return muted;
}

static int32_t conceal_lost(media_buffer* stream, struct silk_codec* codec, struct list_head* head)
{
    int32_t nr = (int32_t) (stream->seq - codec->seq[1]);
    if (codec->seq[1] == 0 || __builtin_expect(nr <= 0 || nr > 24, 1)) {
        return 0;
    }

    SKP_int16 nr_samples;
    uint64_t pts = stream->pts;
    stream->pts = codec->pts;
    uint32_t ms = 0, bytes = 0;

    for (int32_t i = 0; i < nr; ++i) {
        struct my_buffer* buffer = pcm_buffer_alloc(NULL, ms, stream, codec);
        if (buffer == NULL) {
            break;
        }

        SKP_Silk_SDK_Decode(codec->obj, &codec->control, 1,
                            NULL, 0, (SKP_int16 *) buffer->ptr[1], &nr_samples);
        my_assert(nr_samples * 2 == buffer->length);
        bytes += buffer->length;
        list_add_tail(&buffer->head, head);
        ms += silk_mpf;
    }

    stream->pts = pts;
    return bytes;
}

static int32_t silk_write(void* handle, struct my_buffer* mbuf, struct list_head* head, int32_t* delay)
{
    (void) delay;
    if (__builtin_expect(mbuf == NULL, 0)) {
        return 0;
    }

    struct silk_codec* codec = (struct silk_codec *) handle;
    media_buffer* stream = (media_buffer *) mbuf->ptr[0];
    uint32_t ms = 0;
    SKP_int16 nr_samples;
    int32_t bytes = conceal_lost(stream, codec, head);

    while (mbuf->length > 0) {
        struct my_buffer* buffer = NULL;
        uint32_t muted = silk_muted(mbuf, silk_frame_leading);

        uint16_t len = silk_read_frame_bytes(mbuf->ptr[1]);
        mbuf->ptr[1] += silk_frame_leading;
        if (muted) {
            buffer = codec->fmt_out->ops->muted_frame_get(codec->fmt_out, silk_mpf);
            if (__builtin_expect(buffer != NULL, 1)) {
                my_assert(buffer->length == codec->bytes);
                bytes += buffer->length;

                buffer = pcm_buffer_alloc(buffer, ms, stream, codec);
                list_add_tail(&buffer->head, head);
            }
        } else {
            buffer = pcm_buffer_alloc(buffer, ms, stream, codec);
            if (__builtin_expect(buffer != NULL, 1)) {
                SKP_Silk_SDK_Decode(codec->obj, &codec->control, 0,
                                    (const SKP_uint8 *) mbuf->ptr[1], (const SKP_int) len,
                                    (SKP_int16 *) buffer->ptr[1], &nr_samples);
                bytes += nr_samples * 2;
                list_add_tail(&buffer->head, head);
            }
        }

        mbuf->ptr[1] += len;
        mbuf->length -= (len + silk_frame_leading);
        ms += silk_mpf;
    }

    codec->seq[1] = stream->seq + 1;
    codec->pts = stream->pts + ms;
    my_assert(mbuf->length == 0);
    mbuf->mop->free(mbuf);
    return bytes;
}

static void silk_close(void* handle)
{
    struct silk_codec* codec = (struct silk_codec *) handle;
    if (codec->mbuf_handle != -1) {
        mbuf_reap(codec->mbuf_handle);
    }
    my_free(codec);
}

static mpu_operation silk_ops = {
    silk_open,
    silk_write,
    silk_close
};

media_process_unit silk_dec_16k16b1 = {
    &silk_16k16b1.cc,
    &pcm_16k16b1.cc,
    &silk_ops,
    1,
    mpu_decoder,
    "silk_dec_16k16b1"
};

media_process_unit silk_dec_8k16b1 = {
    &silk_8k16b1.cc,
    &pcm_8k16b1.cc,
    &silk_ops,
    1,
    mpu_decoder,
    "silk_dec_8k16b1"
};
