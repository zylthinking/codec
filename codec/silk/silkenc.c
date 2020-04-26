
#include "mpu.h"
#include "mbuf.h"
#include "media_buffer.h"
#include "my_errno.h"
#include "mem.h"
#include "silk.h"
#include "interface/SKP_Silk_SDK_API.h"
#include <string.h>

struct silk_codec {
    void* obj;
    SKP_SILK_SDK_EncControlStruct control;
    audio_format* fmt_out;
    uint32_t align, seq;

    struct my_buffer* mbuf;
    intptr_t mbuf_handle;
};

static void* silk_open(fourcc** in, fourcc** out)
{
    SKP_int32 size;
    audio_format* fmt_in = to_audio_format(in);
    fraction bytes_per_ms = pcm_bytes_from_ms(fmt_in->pcm, 1);

    SKP_int n = SKP_Silk_SDK_Get_Encoder_Size(&size);
    if (n != 0) {
        errno = EFAULT;
        return NULL;
    }
    
    struct silk_codec* codec = (struct silk_codec *) my_malloc(size + sizeof(struct silk_codec));
    if (codec == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    audio_format* fmt_out = to_audio_format(out);
    struct silk_param* param = *(struct silk_param **) fmt_out->pparam;
    codec->mbuf = NULL;
    codec->obj = (void *) (codec + 1);
    codec->align = silk_mpf * bytes_per_ms.num / bytes_per_ms.den;
    codec->seq = 0;
    codec->mbuf_handle = mbuf_hget(codec->align + sizeof(media_buffer) + silk_frame_leading, 4, 1);
    codec->fmt_out = fmt_out;

    n = SKP_Silk_SDK_InitEncoder(codec->obj, &codec->control);
    if (n != 0) {
        my_free(codec);
        errno = EFAULT;
        return NULL;
    }

    codec->control.API_sampleRate = fmt_in->pcm->samrate;
	codec->control.maxInternalSampleRate = fmt_in->pcm->samrate;
	codec->control.packetSize = codec->align * 8 / fmt_in->pcm->sambits;
	codec->control.packetLossPercentage = 0;
	codec->control.useInBandFEC = 0;
	codec->control.useDTX = 0;
	codec->control.complexity = 0;
	codec->control.bitRate = param->bitrate;
    return codec;
}

static struct my_buffer* pcm_buffer_alloc(struct my_buffer* mbuf,
                                          uint32_t ms, media_buffer* stream, struct silk_codec* codec)
{
    if (mbuf == NULL) {
        if (__builtin_expect(codec->mbuf_handle == -1, 0)) {
            uint32_t bytes = sizeof(media_buffer) + silk_frame_leading + codec->align;
            codec->mbuf_handle = mbuf_hget(bytes, 4, 1);
            if (codec->mbuf_handle == -1) {
                mbuf = mbuf_alloc_2(bytes);
            }
        } else {
            mbuf = mbuf_alloc_1(codec->mbuf_handle);
        }

        if (mbuf == NULL) {
            return NULL;
        }

        mbuf->length = codec->align;
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
    media->seq = codec->seq++;

    media->vp[0].ptr = mbuf->ptr[1];
    media->vp[0].stride = (uint32_t) mbuf->length;
    return mbuf;
}

static void save_padding(struct silk_codec* codec, struct my_buffer* mbuf, uint32_t ms)
{
    my_assert(codec->mbuf == NULL);
    media_buffer* stream = (media_buffer *) mbuf->ptr[0];
    codec->mbuf = pcm_buffer_alloc(NULL, ms, stream, codec);
    if (codec->mbuf != NULL) {
        memcpy(codec->mbuf->ptr[1], mbuf->ptr[1], mbuf->length);
        codec->mbuf->ptr[1] += mbuf->length;
        codec->mbuf->length -= mbuf->length;
        mbuf->length = 0;
    }
}

static uint32_t pcm_muted(struct my_buffer* mbuf, uint32_t bytes)
{
    uintptr_t nb = mbuf->length;
    mbuf->length = bytes;
    uint32_t muted = silk_frame_muted(mbuf);
    mbuf->length = nb;
    return muted;
}

static int32_t do_encode(struct silk_codec* codec, struct my_buffer* mbuf, struct list_head* head)
{
    uint32_t ms = 0;
    int32_t bytes = 0;
    int samples = codec->align / 2;
    media_buffer* stream = (media_buffer *) mbuf->ptr[0];

    while (mbuf->length >= codec->align) {
        SKP_int16 output_nb = codec->align;
        struct my_buffer* outbuf = NULL;
        if (pcm_muted(mbuf, output_nb)) {
            outbuf = codec->fmt_out->ops->muted_frame_get(codec->fmt_out, silk_mpf);
            if (outbuf != NULL) {
                outbuf = pcm_buffer_alloc(outbuf, ms, stream, codec);
                bytes += outbuf->length;
                list_add_tail(&outbuf->head, head);
            }
        } else {
            outbuf = pcm_buffer_alloc(outbuf, ms, stream, codec);
            if (outbuf != NULL) {
                SKP_uint8* buffer = (SKP_uint8 *) (outbuf->ptr[1] + silk_frame_leading);
                SKP_int ret = SKP_Silk_SDK_Encode(codec->obj, &codec->control,
                                                  (const SKP_int16 *) mbuf->ptr[1],
                                                  (const SKP_int) samples, buffer, &output_nb);
                my_assert(ret == 0);
                my_assert(codec->align >= (uint32_t) output_nb);

                silk_write_frame_bytes((uint16_t *) outbuf->ptr[1], output_nb);
                outbuf->length = output_nb + silk_frame_leading;
                bytes += outbuf->length;
                list_add_tail(&outbuf->head, head);
            }
        }

        mbuf->ptr[1] += codec->align;
        mbuf->length -= codec->align;
        ms += silk_mpf;
    }

    if (mbuf->length > 0) {
        save_padding(codec, mbuf, ms);
    }
    mbuf->mop->free(mbuf);
    return bytes;
}

static int32_t encode_padding(struct silk_codec* codec, struct my_buffer* mbuf, struct list_head* head)
{
    int32_t bytes = 0;
    if (codec->mbuf == NULL || codec->mbuf->length == codec->align) {
        return bytes;
    }

    media_buffer* stream = (media_buffer *) codec->mbuf->ptr[0];
    uintptr_t need = my_min(codec->mbuf->length, mbuf->length);
    memcpy(codec->mbuf->ptr[1], mbuf->ptr[1], need);
    codec->mbuf->ptr[1] += need;
    codec->mbuf->length -= need;
    mbuf->ptr[1] += need;
    mbuf->length -= need;

    if (codec->mbuf->length == 0) {
        uint64_t pts = stream->pts + silk_mpf;

        stream = (media_buffer *) mbuf->ptr[0];
        if (__builtin_expect(pts > stream->pts, 1)) {
            stream->pts = pts;
        }

        codec->mbuf->ptr[1] -= codec->align;
        codec->mbuf->length = codec->align;
        bytes = do_encode(codec, codec->mbuf, head);
        codec->mbuf = NULL;
    }
    return bytes;
}

static int32_t silk_write(void* handle, struct my_buffer* mbuf, struct list_head* head, int32_t* delay)
{
    (void) delay;
    if (__builtin_expect(mbuf == NULL, 0)) {
        return 0;
    }

    struct silk_codec* codec = (struct silk_codec *) handle;
    int32_t bytes = encode_padding(codec, mbuf, head);

    if (mbuf->length > 0) {
        bytes += do_encode(codec, mbuf, head);
    } else {
        mbuf->mop->free(mbuf);
    }

    return bytes;
}

static void silk_close(void* handle)
{
    struct silk_codec* codec = (struct silk_codec *) handle;
    if (codec->mbuf_handle != -1) {
        mbuf_reap(codec->mbuf_handle);
    }

    if (codec->mbuf != NULL) {
        codec->mbuf->mop->free(codec->mbuf);
    }
    my_free(codec);
}

static mpu_operation silk_ops = {
    silk_open,
    silk_write,
    silk_close
};

media_process_unit silk_enc_16k16b1 = {
    &pcm_16k16b1.cc,
    &silk_16k16b1.cc,
    &silk_ops,
    1,
    mpu_encoder,
    "silk_enc_16k16b1"
};

media_process_unit silk_enc_8k16b1 = {
    &pcm_8k16b1.cc,
    &silk_8k16b1.cc,
    &silk_ops,
    1,
    mpu_encoder,
    "silk_enc_8k16b1"
};
