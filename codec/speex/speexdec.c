
#include "mpu.h"
#include "mbuf.h"
#include "media_buffer.h"
#include "my_errno.h"
#include "mem.h"
#include "include/speex/speex.h"
#include "include/speex/speex_header.h"
#include "include/speex/speex_stereo.h"

static int frame_bytes[2][11] = {
    {6, 10, 15, 20, 20, 28, 28, 38, 38, 46, 62},
    {8, 12, 17, 23, 23, 30, 30, 40, 40, 48, 64}
};

struct speex_codec {
    void* obj;
    fourcc** fourcc_out;
    SpeexBits bits;
    SpeexStereoState stereo;
    uint64_t seq;

    intptr_t mbuf_handle;
    uint32_t bytes, input_bytes;
    uint32_t output_samples, channel;
};

static void* speex_open(fourcc** in, fourcc** out)
{
    struct speex_codec* codec = (struct speex_codec *) my_malloc(sizeof(struct speex_codec));
    if (codec == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    codec->fourcc_out = out;
    codec->seq = 0;
    audio_format* fmt = to_audio_format(in);

    int mode_id;
	if (fmt->pcm->samrate > 25000) {
		mode_id = SPEEX_MODEID_UWB;
	} else if (fmt->pcm->samrate > 12500) {
		mode_id = SPEEX_MODEID_WB;
	} else {
		mode_id = SPEEX_MODEID_NB;
	}

	const SpeexMode* mode = speex_lib_get_mode(mode_id);
    codec->obj = speex_decoder_init(mode);
	if (codec->obj == NULL) {
        my_free(codec);
        errno = EFAILED;
		return NULL;
	}

    int enable = 1;
	speex_decoder_ctl(codec->obj, SPEEX_SET_ENH, &enable);
	speex_decoder_ctl(codec->obj, SPEEX_SET_SAMPLING_RATE, &fmt->pcm->samrate);
	speex_decoder_ctl(codec->obj, SPEEX_SET_HIGHPASS, &enable);
	speex_decoder_ctl(codec->obj, SPEEX_GET_FRAME_SIZE, &codec->output_samples);
	speex_bits_init(&codec->bits);
	speex_stereo_state_reset(&codec->stereo);

    unsigned char level = **((unsigned char **) fmt->pparam);
    codec->input_bytes = frame_bytes[fmt->pcm->channel - 1][level];
    fmt = to_audio_format(out);
    codec->channel = fmt->pcm->channel;
    codec->bytes = codec->output_samples * fmt->pcm->channel * fmt->pcm->sambits / 8;
    codec->mbuf_handle = mbuf_hget(sizeof(media_buffer) + codec->bytes, 8, 1);
    return codec;
}

static int do_decode(struct speex_codec* codec, char* in, int in_bytes, char* out)
{
	SpeexBits* st;
	if (in_bytes != 0) {
		speex_bits_read_from(&codec->bits, in, in_bytes);
		st = &codec->bits;
	} else {
		st = NULL;
	}

	if (speex_decode_int(codec->obj, st, (short *) out) == 0) {
		if (codec->channel == 2) {
			speex_decode_stereo_int((short*) out, codec->output_samples, &codec->stereo);
		}
		return 0;
	}
	return -1;
}

static struct my_buffer* pcm_buffer_alloc(uint32_t ms, media_buffer* stream, struct speex_codec* codec)
{
    struct my_buffer* mbuf = NULL;
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
    mbuf->ptr[1] = (char*) mbuf->ptr[0] + sizeof(media_buffer);

    media_buffer* media = (media_buffer *) mbuf->ptr[0];
    memset(media->vp, 0, sizeof(media->vp));
    media->frametype = 0;
    media->angle = 0;
    media->fragment[0] = 0;
    media->fragment[1] = 1;
    media->pptr_cc = codec->fourcc_out;
    media->seq = codec->seq++;
    media->pts = stream->pts + ms;
    media->iden = stream->iden;
    media->vp[0].ptr = mbuf->ptr[1];
    media->vp[0].stride = (uint32_t) mbuf->length;
    return mbuf;
}

static int32_t speex_write(void* any, struct my_buffer* mbuf, struct list_head* headp, int32_t* delay)
{
    (void) delay;
    if (__builtin_expect(mbuf == NULL, 0)) {
        return 0;
    }

    int32_t bytes = 0;
    struct speex_codec* codec = (struct speex_codec *) any;
    media_buffer* stream = (media_buffer *) mbuf->ptr[0];
    struct my_buffer* buffer = NULL;
    uint32_t ms = 0;

    while (mbuf->length > 0) {
        if (__builtin_expect(buffer == NULL, 1)) {
            buffer = pcm_buffer_alloc(ms, stream, codec);
        }

        uint16_t frame_bytes = codec->input_bytes;
        if (__builtin_expect(buffer != NULL, 1)) {
            int n = do_decode(codec, mbuf->ptr[1], frame_bytes, buffer->ptr[1]);
            if (__builtin_expect(n == 0, 1)) {
                bytes += codec->bytes;
                list_add_tail(&buffer->head, headp);
                buffer = NULL;
            }
        }

        mbuf->ptr[1] += frame_bytes;
        mbuf->length -= frame_bytes;
        ms += 20;
    }

    my_assert(mbuf->length == 0);
    if (buffer != NULL) {
        buffer->mop->free(buffer);
    }
    mbuf->mop->free(mbuf);
    return bytes;
}

static void speex_close(void* any)
{
    struct speex_codec* codec = (struct speex_codec *) any;
	speex_decoder_destroy(codec->obj);
	speex_bits_destroy(&codec->bits);

    if (codec->mbuf_handle != -1) {
        mbuf_reap(codec->mbuf_handle);
    }
    my_free(codec);
}

static mpu_operation speex_ops = {
    speex_open,
    speex_write,
    speex_close
};

media_process_unit speex_dec_8k16b1_mode8 = {
    &speex_8k16b1_mode8.cc,
    &pcm_8k16b1.cc,
    &speex_ops,
    1,
    mpu_decoder,
    "speex_dec_8k16b1_mode8"
};
