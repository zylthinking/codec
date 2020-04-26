
#include "mpu.h"
#include "mbuf.h"
#include "media_buffer.h"
#include "my_errno.h"
#include "mem.h"
#include "include/speex/speex_resampler.h"
#include <string.h>

typedef struct {
    SpeexResamplerState* stat;
    pcm_format* pcm[2];
    uint32_t align;
    uint32_t nr;
    // 48K-->8k may cache 5 samples at max, 12 samples should be engough
    uint8_t saved[48];
    void (*stero_2_mono) (struct my_buffer*);
    void (*mono_2_stero) (struct my_buffer*);
} speex_resampler;

static void mono_2_stero(struct my_buffer* buf)
{
    int len = (int) buf->length;
    short* src = (short *) buf->ptr[1];
    short* dst = (short *) (buf->ptr[1] - len);

    int i, j, len2 = len - 8;
    for (i = 0, j = 0; i < len2; i += 8, j += 4) {
        dst[i] = dst[i + 1] = src[j];
        dst[i + 2] = dst[i + 3] = src[j + 1];
        dst[i + 4] = dst[i + 5] = src[j + 2];
        dst[i + 6] = dst[i + 7] = src[j + 3];
    }

    for (; i < len; i += 2, j += 1) {
        dst[i] = dst[i + 1] = src[j];
    }

    buf->ptr[1] = (char *) dst;
    buf->length = len * 2;
}

static void stero_2_mono(struct my_buffer* buf)
{
    int len = (int) buf->length >> 2;
    short* src = (short *) buf->ptr[1];
    short* dst = src;

    int i, j, len2 = len - 4;
    for (i = 0, j = 0; i < len2; i += 4, j += 8) {
        dst[i] = src[j];
        dst[i + 1] = src[j + 2];
        dst[i + 2] = src[j + 4];
        dst[i + 3] = src[j + 6];
    }

    for (; i < len; i += 1, j += 2) {
        dst[i] = src[j];
    }

    buf->length = len * 2;
}

static void* resampler_open(fourcc** in, fourcc** out)
{
    audio_format* fmt_in = to_audio_format(in);
    audio_format* fmt_out = to_audio_format(out);
    my_assert(fmt_in != fmt_out);

    speex_resampler* resampler = (speex_resampler *) my_malloc(sizeof(speex_resampler));
    if (resampler == NULL) {
        return NULL;
    }

    resampler->stat = NULL;
    resampler->stero_2_mono = NULL;
    resampler->mono_2_stero = NULL;

    if (fmt_in->pcm->samrate != fmt_out->pcm->samrate) {
        spx_uint32_t chans = 1;
        if (fmt_in->pcm->channel == fmt_out->pcm->channel) {
            chans = fmt_in->pcm->channel;
        }

        SpeexResamplerState* stat = speex_resampler_init(chans,
                                        fmt_in->pcm->samrate, fmt_out->pcm->samrate, 6, NULL);
        if (stat == NULL) {
            my_free(resampler);
            return NULL;
        }
        resampler->stat = stat;
    }

    resampler->nr = 0;
    resampler->pcm[0] = fmt_out->pcm;
    resampler->pcm[1] = fmt_in->pcm;
    resampler->align = fmt_in->pcm->channel * fmt_in->pcm->sambits / 8;

    if (fmt_in->pcm->channel == 2 && fmt_out->pcm->channel == 1) {
        resampler->stero_2_mono = stero_2_mono;
        resampler->align /= 2;
        resampler->mono_2_stero = NULL;
    } else if (fmt_in->pcm->channel == 1 && fmt_out->pcm->channel == 2) {
        resampler->stero_2_mono = NULL;
        resampler->mono_2_stero = mono_2_stero;
    } else {
        my_assert(resampler->stat != NULL);
    }
    return resampler;
}

static int32_t resampler_write(void* any, struct my_buffer* mbuf, struct list_head* headp, int32_t* delay)
{
    (void) delay;
    if (__builtin_expect(mbuf == NULL, 0)) {
        return 0;
    }

    speex_resampler* resampler = (speex_resampler *) any;
    if (resampler->stero_2_mono != NULL) {
        resampler->stero_2_mono(mbuf);
        if (resampler->stat == NULL) {
            list_add_tail(&mbuf->head, headp);
            return (int32_t) mbuf->length;
        }
    }

    uint32_t nb = (uint32_t) (((mbuf->length + resampler->nr) * resampler->pcm[0]->samrate) / resampler->pcm[1]->samrate);
    nb += 1;
    uint32_t bytes = nb;
    if (resampler->mono_2_stero != NULL) {
        bytes *= 2;
    }

    struct my_buffer* buf = mbuf_alloc_2(bytes + sizeof(media_buffer));
    if (buf == NULL) {
        mbuf->mop->free(mbuf);
        return 0;
    }

    if (resampler->mono_2_stero == NULL) {
        buf->ptr[1] = buf->ptr[0] + sizeof(media_buffer);
        buf->length -= sizeof(media_buffer);
    } else {
        buf->ptr[1] = buf->ptr[0] + sizeof(media_buffer) + nb;
        buf->length -= (sizeof(media_buffer) + nb);
    }

    media_buffer* mb = (media_buffer *) buf->ptr[0];
    *mb = *(media_buffer *) mbuf->ptr[0];
    if (resampler->nr > 0) {
        mbuf->ptr[1] -= resampler->nr;
        mbuf->length += resampler->nr;
        memcpy(mbuf->ptr[1], resampler->saved, resampler->nr);
    }

    if (resampler->stat != NULL) {
        spx_uint32_t out = (spx_uint32_t) (buf->length / resampler->align);
        uint32_t blocks = (uint32_t) (mbuf->length / resampler->align);
        uint32_t in = blocks;
        // this function will never failed indeed.
        speex_resampler_process_interleaved_int(
                resampler->stat,
                (const spx_int16_t *) mbuf->ptr[1],
                &in,
                (spx_int16_t *) buf->ptr[1],
                &out
        );

        buf->length = out * resampler->align;
        if (in < blocks) {
            resampler->nr = resampler->align * (blocks - in);
            my_assert(resampler->nr <= sizeof(resampler->saved));
            memcpy(
                resampler->saved,
                mbuf->ptr[1] + in * resampler->align,
                resampler->nr
            );
        } else {
            resampler->nr = 0;
        }
    } else {
        memcpy(buf->ptr[1], mbuf->ptr[1], mbuf->length);
        buf->length = mbuf->length;
    }
    mbuf->mop->free(mbuf);

    if (resampler->mono_2_stero != NULL) {
        resampler->mono_2_stero(buf);
    }
    list_add_tail(&buf->head, headp);
    return (int32_t) buf->length;
}

static void resampler_close(void* any)
{
    speex_resampler* resampler = (speex_resampler *) any;
    if (resampler->stat != NULL) {
        speex_resampler_destroy(resampler->stat);
    }
    my_free(any);
}

static mpu_operation reampler_ops = {
    resampler_open,
    resampler_write,
    resampler_close
};

#define define_resampler(in, out) \
media_process_unit resampler_##in##_##out = { \
    &pcm_##in.cc, \
    &pcm_##out.cc, \
    &reampler_ops, \
    1, \
    mpu_convert, \
    "resampler_"#in"_"#out \
};

define_resampler(8k16b1, 8k16b2);
define_resampler(8k16b1, 11k16b1);
define_resampler(8k16b1, 11k16b2);
define_resampler(8k16b1, 16k16b1);
define_resampler(8k16b1, 16k16b2);
define_resampler(8k16b1, 22k16b1);
define_resampler(8k16b1, 22k16b2);
define_resampler(8k16b1, 24k16b1);
define_resampler(8k16b1, 24k16b2);
define_resampler(8k16b1, 32k16b1);
define_resampler(8k16b1, 32k16b2);
define_resampler(8k16b1, 44k16b1);
define_resampler(8k16b1, 44k16b2);
define_resampler(8k16b1, 48k16b1);
define_resampler(8k16b1, 48k16b2);

define_resampler(8k16b2, 8k16b1);
define_resampler(8k16b2, 11k16b1);
define_resampler(8k16b2, 11k16b2);
define_resampler(8k16b2, 16k16b1);
define_resampler(8k16b2, 16k16b2);
define_resampler(8k16b2, 22k16b1);
define_resampler(8k16b2, 22k16b2);
define_resampler(8k16b2, 24k16b1);
define_resampler(8k16b2, 24k16b2);
define_resampler(8k16b2, 32k16b1);
define_resampler(8k16b2, 32k16b2);
define_resampler(8k16b2, 44k16b1);
define_resampler(8k16b2, 44k16b2);
define_resampler(8k16b2, 48k16b1);
define_resampler(8k16b2, 48k16b2);

define_resampler(11k16b1, 11k16b2);
define_resampler(11k16b1, 8k16b1);
define_resampler(11k16b1, 8k16b2);
define_resampler(11k16b1, 16k16b1);
define_resampler(11k16b1, 16k16b2);
define_resampler(11k16b1, 22k16b1);
define_resampler(11k16b1, 22k16b2);
define_resampler(11k16b1, 24k16b1);
define_resampler(11k16b1, 24k16b2);
define_resampler(11k16b1, 32k16b1);
define_resampler(11k16b1, 32k16b2);
define_resampler(11k16b1, 44k16b1);
define_resampler(11k16b1, 44k16b2);
define_resampler(11k16b1, 48k16b1);
define_resampler(11k16b1, 48k16b2);

define_resampler(11k16b2, 11k16b1);
define_resampler(11k16b2, 8k16b1);
define_resampler(11k16b2, 8k16b2);
define_resampler(11k16b2, 16k16b1);
define_resampler(11k16b2, 16k16b2);
define_resampler(11k16b2, 22k16b1);
define_resampler(11k16b2, 22k16b2);
define_resampler(11k16b2, 24k16b1);
define_resampler(11k16b2, 24k16b2);
define_resampler(11k16b2, 32k16b1);
define_resampler(11k16b2, 32k16b2);
define_resampler(11k16b2, 44k16b1);
define_resampler(11k16b2, 44k16b2);
define_resampler(11k16b2, 48k16b1);
define_resampler(11k16b2, 48k16b2);

define_resampler(16k16b1, 16k16b2);
define_resampler(16k16b1, 8k16b1);
define_resampler(16k16b1, 8k16b2);
define_resampler(16k16b1, 11k16b1);
define_resampler(16k16b1, 11k16b2);
define_resampler(16k16b1, 22k16b1);
define_resampler(16k16b1, 22k16b2);
define_resampler(16k16b1, 24k16b1);
define_resampler(16k16b1, 24k16b2);
define_resampler(16k16b1, 32k16b1);
define_resampler(16k16b1, 32k16b2);
define_resampler(16k16b1, 44k16b1);
define_resampler(16k16b1, 44k16b2);
define_resampler(16k16b1, 48k16b1);
define_resampler(16k16b1, 48k16b2);

define_resampler(16k16b2, 16k16b1);
define_resampler(16k16b2, 8k16b1);
define_resampler(16k16b2, 8k16b2);
define_resampler(16k16b2, 11k16b1);
define_resampler(16k16b2, 11k16b2);
define_resampler(16k16b2, 22k16b1);
define_resampler(16k16b2, 22k16b2);
define_resampler(16k16b2, 24k16b1);
define_resampler(16k16b2, 24k16b2);
define_resampler(16k16b2, 32k16b1);
define_resampler(16k16b2, 32k16b2);
define_resampler(16k16b2, 44k16b1);
define_resampler(16k16b2, 44k16b2);
define_resampler(16k16b2, 48k16b1);
define_resampler(16k16b2, 48k16b2);

define_resampler(22k16b1, 22k16b2);
define_resampler(22k16b1, 8k16b1);
define_resampler(22k16b1, 8k16b2);
define_resampler(22k16b1, 11k16b1);
define_resampler(22k16b1, 11k16b2);
define_resampler(22k16b1, 16k16b1);
define_resampler(22k16b1, 16k16b2);
define_resampler(22k16b1, 24k16b1);
define_resampler(22k16b1, 24k16b2);
define_resampler(22k16b1, 32k16b1);
define_resampler(22k16b1, 32k16b2);
define_resampler(22k16b1, 44k16b1);
define_resampler(22k16b1, 44k16b2);
define_resampler(22k16b1, 48k16b1);
define_resampler(22k16b1, 48k16b2);

define_resampler(22k16b2, 22k16b1);
define_resampler(22k16b2, 8k16b1);
define_resampler(22k16b2, 8k16b2);
define_resampler(22k16b2, 11k16b1);
define_resampler(22k16b2, 11k16b2);
define_resampler(22k16b2, 16k16b1);
define_resampler(22k16b2, 16k16b2);
define_resampler(22k16b2, 24k16b1);
define_resampler(22k16b2, 24k16b2);
define_resampler(22k16b2, 32k16b1);
define_resampler(22k16b2, 32k16b2);
define_resampler(22k16b2, 44k16b1);
define_resampler(22k16b2, 44k16b2);
define_resampler(22k16b2, 48k16b1);
define_resampler(22k16b2, 48k16b2);

define_resampler(24k16b1, 24k16b2);
define_resampler(24k16b1, 8k16b1);
define_resampler(24k16b1, 8k16b2);
define_resampler(24k16b1, 11k16b1);
define_resampler(24k16b1, 11k16b2);
define_resampler(24k16b1, 16k16b1);
define_resampler(24k16b1, 16k16b2);
define_resampler(24k16b1, 22k16b1);
define_resampler(24k16b1, 22k16b2);
define_resampler(24k16b1, 32k16b1);
define_resampler(24k16b1, 32k16b2);
define_resampler(24k16b1, 44k16b1);
define_resampler(24k16b1, 44k16b2);
define_resampler(24k16b1, 48k16b1);
define_resampler(24k16b1, 48k16b2);

define_resampler(24k16b2, 24k16b1);
define_resampler(24k16b2, 8k16b1);
define_resampler(24k16b2, 8k16b2);
define_resampler(24k16b2, 11k16b1);
define_resampler(24k16b2, 11k16b2);
define_resampler(24k16b2, 16k16b1);
define_resampler(24k16b2, 16k16b2);
define_resampler(24k16b2, 22k16b1);
define_resampler(24k16b2, 22k16b2);
define_resampler(24k16b2, 32k16b1);
define_resampler(24k16b2, 32k16b2);
define_resampler(24k16b2, 44k16b1);
define_resampler(24k16b2, 44k16b2);
define_resampler(24k16b2, 48k16b1);
define_resampler(24k16b2, 48k16b2);

define_resampler(32k16b1, 32k16b2);
define_resampler(32k16b1, 8k16b1);
define_resampler(32k16b1, 8k16b2);
define_resampler(32k16b1, 11k16b1);
define_resampler(32k16b1, 11k16b2);
define_resampler(32k16b1, 16k16b1);
define_resampler(32k16b1, 16k16b2);
define_resampler(32k16b1, 22k16b1);
define_resampler(32k16b1, 22k16b2);
define_resampler(32k16b1, 24k16b1);
define_resampler(32k16b1, 24k16b2);
define_resampler(32k16b1, 44k16b1);
define_resampler(32k16b1, 44k16b2);
define_resampler(32k16b1, 48k16b1);
define_resampler(32k16b1, 48k16b2);

define_resampler(32k16b2, 32k16b1);
define_resampler(32k16b2, 8k16b1);
define_resampler(32k16b2, 8k16b2);
define_resampler(32k16b2, 11k16b1);
define_resampler(32k16b2, 11k16b2);
define_resampler(32k16b2, 16k16b1);
define_resampler(32k16b2, 16k16b2);
define_resampler(32k16b2, 22k16b1);
define_resampler(32k16b2, 22k16b2);
define_resampler(32k16b2, 24k16b1);
define_resampler(32k16b2, 24k16b2);
define_resampler(32k16b2, 44k16b1);
define_resampler(32k16b2, 44k16b2);
define_resampler(32k16b2, 48k16b1);
define_resampler(32k16b2, 48k16b2);

define_resampler(44k16b1, 44k16b2);
define_resampler(44k16b1, 8k16b1);
define_resampler(44k16b1, 8k16b2);
define_resampler(44k16b1, 11k16b1);
define_resampler(44k16b1, 11k16b2);
define_resampler(44k16b1, 16k16b1);
define_resampler(44k16b1, 16k16b2);
define_resampler(44k16b1, 22k16b1);
define_resampler(44k16b1, 22k16b2);
define_resampler(44k16b1, 24k16b1);
define_resampler(44k16b1, 24k16b2);
define_resampler(44k16b1, 32k16b1);
define_resampler(44k16b1, 32k16b2);
define_resampler(44k16b1, 48k16b1);
define_resampler(44k16b1, 48k16b2);

define_resampler(44k16b2, 44k16b1);
define_resampler(44k16b2, 8k16b1);
define_resampler(44k16b2, 8k16b2);
define_resampler(44k16b2, 11k16b1);
define_resampler(44k16b2, 11k16b2);
define_resampler(44k16b2, 16k16b1);
define_resampler(44k16b2, 16k16b2);
define_resampler(44k16b2, 22k16b1);
define_resampler(44k16b2, 22k16b2);
define_resampler(44k16b2, 24k16b1);
define_resampler(44k16b2, 24k16b2);
define_resampler(44k16b2, 32k16b1);
define_resampler(44k16b2, 32k16b2);
define_resampler(44k16b2, 48k16b1);
define_resampler(44k16b2, 48k16b2);

define_resampler(48k16b1, 48k16b2);
define_resampler(48k16b1, 8k16b1);
define_resampler(48k16b1, 8k16b2);
define_resampler(48k16b1, 11k16b1);
define_resampler(48k16b1, 11k16b2);
define_resampler(48k16b1, 16k16b1);
define_resampler(48k16b1, 16k16b2);
define_resampler(48k16b1, 22k16b1);
define_resampler(48k16b1, 22k16b2);
define_resampler(48k16b1, 24k16b1);
define_resampler(48k16b1, 24k16b2);
define_resampler(48k16b1, 32k16b1);
define_resampler(48k16b1, 32k16b2);
define_resampler(48k16b1, 44k16b1);
define_resampler(48k16b1, 44k16b2);

define_resampler(48k16b2, 48k16b1);
define_resampler(48k16b2, 8k16b1);
define_resampler(48k16b2, 8k16b2);
define_resampler(48k16b2, 11k16b1);
define_resampler(48k16b2, 11k16b2);
define_resampler(48k16b2, 16k16b1);
define_resampler(48k16b2, 16k16b2);
define_resampler(48k16b2, 22k16b1);
define_resampler(48k16b2, 22k16b2);
define_resampler(48k16b2, 24k16b1);
define_resampler(48k16b2, 24k16b2);
define_resampler(48k16b2, 32k16b1);
define_resampler(48k16b2, 32k16b2);
define_resampler(48k16b2, 44k16b1);
define_resampler(48k16b2, 44k16b2);
