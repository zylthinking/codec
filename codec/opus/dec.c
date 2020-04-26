
#include "mpu.h"
#include "mbuf.h"
#include "media_buffer.h"
#include "my_errno.h"
#include "mem.h"
#include "include/opus.h"
#include <string.h>

struct opus_codec {
    void* obj;
    uint64_t seq[2], pts;

    audio_format* fmt_out;
    intptr_t mbuf_handle;
    uint32_t sample_bytes, bytes;

    uint32_t buf_size, max_frame_bytes;
    unsigned char buffer[512];
};

static void* opus_open(fourcc** in, fourcc** out)
{
    audio_format* fmt = to_audio_format(in);
    int size = opus_decoder_get_size(fmt->pcm->channel);
    struct opus_codec* codec = (struct opus_codec *) my_malloc(size + sizeof(struct opus_codec));
    if (codec == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    codec->obj = (void *) (codec + 1);

    int n = opus_decoder_init(codec->obj, fmt->pcm->samrate, fmt->pcm->channel);
    if (n != 0) {
        my_free(codec);
        errno = EFAULT;
        return NULL;
    }

    codec->buf_size = 0;
    codec->max_frame_bytes = 0;
    codec->seq[0] = codec->seq[1] = codec->pts = 0;
    codec->fmt_out = to_audio_format(out);
    // 因为采样率被固定为有限几个, 可以断定以下计算不会产生精度丢失, 并且 codec->bytes 是 96 的整数倍
    codec->sample_bytes = (fmt->pcm->sambits / 8) * fmt->pcm->channel;
    codec->bytes = (fmt->pcm->samrate * 120 / 1000) * codec->sample_bytes;
    codec->mbuf_handle = mbuf_hget(sizeof(media_buffer) + codec->bytes, 8, 1);
    return codec;
}

static struct my_buffer* pcm_buffer_alloc(struct my_buffer* mbuf,
                                          uint32_t ms, media_buffer* stream, struct opus_codec* codec)
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
    media->pptr_cc = &codec->fmt_out->cc;
    media->pts = stream->pts + ms;
    media->iden = stream->iden;
    media->seq = codec->seq[0]++;
    media->fragment[0] = 0;
    media->fragment[1] = 1;
    media->frametype = 0;
    media->angle = 0;
    media->vp[0].ptr = mbuf->ptr[1];
    media->vp[0].stride = (uint32_t) mbuf->length;
    return mbuf;
}

static int32_t conceal_lost(media_buffer* stream, struct opus_codec* codec, struct list_head* head)
{
    uint64_t pts = stream->pts;
    my_assert(pts > codec->pts);
    stream->pts = codec->pts;
    uint32_t ms = 0, bytes = 0;
    int frame_size = 0;

    uint64_t lost = pts - codec->pts;
    do {
        struct my_buffer* buffer = pcm_buffer_alloc(NULL, ms, stream, codec);
        if (buffer == NULL) {
            break;
        }

        intptr_t nb = buffer->length;
        if (lost >= 120) {
            frame_size = (int) nb;
            lost -= 120;
        } else {
            frame_size = (int) ((lost * 2 / 5) * (nb / 48));
            lost = 0;
        }

        int samples = frame_size / codec->sample_bytes;
        frame_size = opus_decode((OpusDecoder *) codec->obj, NULL, 0,
                                 (opus_int16 *) buffer->ptr[1], samples, 0);
        if (frame_size < 0) {
            logmsg("opus PLC %d samples failed, error = \n", samples, frame_size);
            buffer->mop->free(buffer);
            break;
        }
        my_assert(frame_size > 0);

        buffer->length = frame_size * codec->sample_bytes;
        bytes += buffer->length;
        list_add_tail(&buffer->head, head);
        ms += 120;
    } while (lost != 0);

    stream->pts = pts;
    return bytes;
}

static int32_t check_seq_discontinuous(media_buffer* stream, struct opus_codec* codec, struct list_head* head)
{
    int32_t nr = (int32_t) (stream->seq - codec->seq[1]);
    if (codec->seq[1] == 0 || __builtin_expect(nr <= 0, 1)) {
        return 0;
    }

    codec->buf_size = 0;
    if (__builtin_expect(nr > 0, 1)) {
        return conceal_lost(stream, codec, head);
    }
    // opus 官方告诉的处理方法是丢弃解码出来的前 80ms pcm
    // 80ms 不足以引起人耳特别注意, 而且这不应该是一个常见情况, 因此直接忽略
    return 0;
}

static intptr_t bitstream_parse(struct opus_codec* codec, struct my_buffer* mbuf, const unsigned char* frames[48], opus_int16 size[48])
{
    const unsigned char* pch = NULL;
    opus_int32 len = 0;
    if (codec->buf_size != 0) {
        intptr_t nb = my_min(mbuf->length, codec->max_frame_bytes);
        memcpy(&codec->buffer[codec->buf_size], mbuf->ptr[1], nb);
        mbuf->ptr[1] += nb;
        mbuf->length -= nb;
        pch = codec->buffer;
        len = (opus_int32) (codec->buf_size + nb);
    } else {
        pch = (const unsigned char *) mbuf->ptr[1];
        len = (opus_int32) mbuf->length;
        mbuf->ptr[1] += mbuf->length;
        mbuf->length = 0;
    }

    intptr_t nr;
    do {
        int payload_offset;
        unsigned char out_toc;
        nr = opus_packet_parse(pch, len, &out_toc, frames, size, &payload_offset);
        if (nr == 0) {
            if (pch == codec->buffer && mbuf->length > 0) {
                intptr_t nb = my_min(mbuf->length, sizeof(codec->buffer) - len);
                // 基于 sizeof(codec->buffer) 应该至少能存放一个 frame 的假设
                my_assert(nb > 0);
                memcpy(&codec->buffer[len], mbuf->ptr[1], nb);
                mbuf->ptr[1] += nb;
                mbuf->length -= nb;
                len += nb;
                continue;
            }

            my_assert(len < sizeof(codec->buffer));
            if (len > codec->max_frame_bytes) {
                codec->max_frame_bytes = len;
            }

            if (codec->buffer != pch) {
                memcpy(codec->buffer, pch, len);
            }
            codec->buf_size = len;
            break;
        }

        my_assert(payload_offset == 0);
        if (codec->buf_size != 0) {
            my_assert(size[0] > codec->buf_size);
            codec->buf_size = 0;
        }

        char* end = (char *) (frames[nr - 1] + size[nr - 1]);
        intptr_t nb = (intptr_t) (mbuf->ptr[1] - end);
        my_assert(nb >= 0);
        mbuf->ptr[1] = end;
        mbuf->length += nb;
    } while (nr == 0);

    return nr;
}

static int32_t opus_write(void* handle, struct my_buffer* mbuf, struct list_head* head, int32_t* delay)
{
    (void) delay;
    if (__builtin_expect(mbuf == NULL, 0)) {
        return 0;
    }

    struct opus_codec* codec = (struct opus_codec *) handle;
    media_buffer* stream = (media_buffer *) mbuf->ptr[0];
    uint32_t ms = 0;
    int32_t bytes = check_seq_discontinuous(stream, codec, head);

    const unsigned char* frames[48];
    opus_int16 size[48];
    while (mbuf->length > 0) {
        intptr_t nr = bitstream_parse(codec, mbuf, frames, size);
        if (nr == 0) {
            break;
        }

        for (intptr_t i = 0; i < nr; ++i) {
            if (size[i] > codec->max_frame_bytes) {
                codec->max_frame_bytes = size[i];
            }

            opus_int16 stock[5760 * 2];
            opus_int16* pcm = stock;
            struct my_buffer* buffer = pcm_buffer_alloc(NULL, ms, stream, codec);
            if (buffer != NULL) {
                pcm = (opus_int16 *) buffer->ptr[1];
            }

            int samples = codec->bytes / codec->sample_bytes;
            samples = opus_decode((OpusDecoder *) codec->obj, frames[i], samples, pcm, samples, 0);
            if (samples < 0) {
                logmsg("opus decode frame failed, frame bytes = %d\n", size[i]);
                continue;
            }

            if (samples > 0) {
                ms += samples * 1000 / codec->fmt_out->pcm->samrate;
            }
        }
    }

    codec->seq[1] = stream->seq + 1;
    codec->pts = stream->pts + ms;
    my_assert(mbuf->length == 0);
    mbuf->mop->free(mbuf);
    return bytes;
}

static void opus_close(void* handle)
{
    struct opus_codec* codec = (struct opus_codec *) handle;
    if (codec->mbuf_handle != -1) {
        mbuf_reap(codec->mbuf_handle);
    }
    my_free(codec);
}

static mpu_operation opus_ops = {
    opus_open,
    opus_write,
    opus_close
};

media_process_unit opus_dec_8k16b1 = {
    &opus_8k16b1.cc,
    &pcm_8k16b1.cc,
    &opus_ops,
    1,
    mpu_decoder,
    "opus_dec_8k16b1"
};

media_process_unit opus_dec_16k16b1 = {
    &opus_16k16b1.cc,
    &pcm_16k16b1.cc,
    &opus_ops,
    1,
    mpu_decoder,
    "opus_dec_16k16b1"
};

media_process_unit opus_dec_24k16b1 = {
    &opus_24k16b1.cc,
    &pcm_24k16b1.cc,
    &opus_ops,
    1,
    mpu_decoder,
    "opus_dec_24k16b1"
};

media_process_unit opus_dec_24k16b2 = {
    &opus_24k16b2.cc,
    &pcm_24k16b2.cc,
    &opus_ops,
    1,
    mpu_decoder,
    "opus_dec_24k16b2"
};

media_process_unit opus_dec_32k16b1 = {
    &opus_32k16b1.cc,
    &pcm_32k16b1.cc,
    &opus_ops,
    1,
    mpu_decoder,
    "opus_dec_32k16b1"
};

media_process_unit opus_dec_32k16b2 = {
    &opus_32k16b2.cc,
    &pcm_32k16b2.cc,
    &opus_ops,
    1,
    mpu_decoder,
    "opus_dec_32k16b2"
};

media_process_unit opus_dec_48k16b1 = {
    &opus_48k16b1.cc,
    &pcm_48k16b1.cc,
    &opus_ops,
    1,
    mpu_decoder,
    "opus_dec_48k16b1"
};

media_process_unit opus_dec_48k16b2 = {
    &opus_48k16b1.cc,
    &pcm_48k16b1.cc,
    &opus_ops,
    1,
    mpu_decoder,
    "opus_dec_48k16b2"
};
