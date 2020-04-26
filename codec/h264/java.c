
#if defined(__ANDROID__)
#include "mydef.h"
#include "my_errno.h"
#include "fmt_in.h"
#include "mpu.h"
#include "lock.h"
#include "mbuf.h"
#include "media_buffer.h"
#include "my_handle.h"
#include "myjni.h"
#include "android.h"
#include "nalu.h"

typedef struct {
    lock_t lck;
    my_handle* handle;
    struct list_head head[2];

    fourcc** out;
    media_iden_t id;
    uint64_t seq;
    int32_t nr[2];
    intptr_t sps;
} java_h264_t;

static void runtime_check_encoder();
static jclass clazz = NULL;
static int32_t lib_open(JavaVM* vm, JNIEnv* env)
{
    clazz = (*env)->FindClass(env, "libmedia/codec");
    clazz = (*env)->NewGlobalRef(env, clazz);
    return 0;
}

static uint32_t color_space_normalize(uint32_t color_space)
{
    int csp = -1;
    if (color_space == csp_nv21ref) {
        csp = csp_nv21;
    } else if (color_space == csp_i420ref) {
        csp = csp_i420;
    } else if (color_space == csp_nv12ref) {
        csp = csp_nv12;
    }
    return csp;
}

static intptr_t encoder_init(my_handle* handle, video_format* fmt, java_code* jcode)
{
    JNIEnv* env = jni_get(jcode, clazz);
    if (env == NULL) {
        return -1;
    }

    jint csp = color_space_normalize(fmt->pixel->csp);
    if (csp == -1) {
        return -1;
    }

    jint result = (*env)->CallStaticIntMethod(env, jcode->clazz, jcode->id, (jlong) (intptr_t) handle,
                                              (jint) fmt->pixel->size->width, (jint) fmt->pixel->size->height, csp,
                                              (jint) fmt->caparam->gop, (jint) fmt->caparam->fps, (jint) fmt->caparam->kbps);
    jni_put(jcode);
    if (result == -1) {
        return -1;
    }
    return 0;
}

static void java_h264_free(void* addr)
{
    java_h264_t* ctx = (java_h264_t *) addr;
    stream_end(ctx->id, ctx->out);
    my_assert(ctx->handle != NULL);
    handle_release(ctx->handle);

    free_buffer(&ctx->head[0]);
    free_buffer(&ctx->head[1]);
    my_free(addr);
}

static void* java_open(fourcc** in, fourcc** out)
{
    errno = ENOMEM;
    java_h264_t* ctx = my_malloc(sizeof(java_h264_t));
    if (ctx == NULL) {
        return NULL;
    }

    my_handle* handle = handle_attach(ctx, java_h264_free);
    if (handle == NULL) {
        my_free(ctx);
        return NULL;
    }
    handle_clone(handle);

    INIT_LIST_HEAD(&ctx->head[0]);
    INIT_LIST_HEAD(&ctx->head[1]);
    ctx->lck = lock_val;
    ctx->handle = handle;
    ctx->out = out;
    ctx->id = media_id_unkown;
    ctx->seq = 0;
    ctx->nr[0] = ctx->nr[1] = 0;
    ctx->sps = 0;

    errno = EFAILED;
    video_format* fmt = to_video_format(in);
    java_code code = {{"libmedia/codec", "encoder_init",  "(JIIIIII)I"}, NULL, NULL, 0};
    intptr_t n = encoder_init(handle, fmt, &code);
    if (n == -1) {
        handle_dettach(handle);
        return NULL;
    }
    return ctx;
}

static int32_t java_write(void* handle, struct my_buffer* mbuf, struct list_head* headp, int32_t* delay)
{
    (void) delay;
    java_h264_t* ctx = (java_h264_t *) handle;
    if (__builtin_expect(media_id_eqal(ctx->id, media_id_unkown) && mbuf != NULL, 0)) {
        media_buffer* media = (media_buffer *) mbuf->ptr[0];
        ctx->id = media->iden;
    }

    int32_t nr = 0;
    lock(&ctx->lck);

    if (mbuf != NULL) {
        ctx->nr[1] += 1;
        list_add_tail(&mbuf->head, &ctx->head[1]);
    }

    if (!list_empty(&ctx->head[0])) {
        list_join(headp, &ctx->head[0]);
        list_del_init(&ctx->head[0]);
        nr = ctx->nr[0];
        ctx->nr[0] = 0;
    }
    unlock(&ctx->lck);
    return nr;
}

static void java_close(void* handle)
{
    java_h264_t* ctx = (java_h264_t *) handle;
    handle_clone(ctx->handle);
    handle_dettach(ctx->handle);
}

static mpu_operation java_ops = {
    java_open,
    java_write,
    java_close
};

media_process_unit java_h264_enc = {
    &raw_0x0.cc,
    &h264_0x0.cc,
    NULL,
    1,
    mpu_encoder,
    "java_h264_enc"
};

static void runtime_check_encoder()
{
    int64_t* feature = android_feature_address();
    video_format* in = video_format_get(codec_pixel, feature[camera_csp], 288, 352);
    video_format* out = video_format_get(codec_h264, csp_any, 288, 352);
    void* handle = java_open(&in->cc, &out->cc);
    if (handle != NULL) {
        java_close(handle);
        java_h264_enc.ops = &java_ops;
        logmsg("hardware h264 encoder with java api enabled\n");
    } else {
        logmsg("hardware h264 encoder with java api disabled\n");
    }
}

void probe_java_h264_enc()
{
    static mpu_operation* mops = NULL;
    if (mops == NULL) {
        if (java_h264_enc.ops == NULL) {
            return;
        }
        mops = java_h264_enc.ops;
    }

    int64_t* feature = android_feature_address();
    if (feature && feature[omx_encode] == 0) {
        java_h264_enc.ops = NULL;
    } else {
        java_h264_enc.ops = mops;
    }
}

static jbyteArray copy_pixels(JNIEnv* env, media_buffer* media, intptr_t width, intptr_t height, intptr_t csp)
{
    video_format* fmt = to_video_format(media->pptr_cc);
    uint32_t color_space = color_space_normalize(fmt->pixel->csp);
    my_assert(csp == csp_nv12 || csp == csp_nv12 || csp == csp_i420);
    my_assert(color_space == csp_nv21 || color_space == csp_i420);

    my_assert(0 == (height & 1));
    my_assert(0 == (width & 1));

    intptr_t nb = width * height * 3 / 2;
    jbyteArray buffer = (*env)->NewByteArray(env, nb);
    if (buffer == NULL) {
        return NULL;
    }

    intptr_t h = my_min(media->vp[0].height, height);
    intptr_t w = my_min(media->vp[0].stride, width);
    jbyte* buf = (*env)->GetByteArrayElements(env, buffer, NULL);
    char* pch = (char *) buf;
    if (csp == color_space) {
        uint32_t div[3] = {1, 4, 4};
        for (int i = 0; i < fmt->pixel->panels; ++i) {
            char* in = media->vp[i].ptr;
            if (width == media->vp[0].stride) {
                intptr_t bytes = width * h;
                memcpy(pch, in, bytes);
            } else {
                char* p = pch;
                for (int i = 0; i < h; ++i) {
                    memcpy(p, in, w);
                    p += width;
                    in += media->vp[i].stride;
                }
            }
            pch += width * height / div[i];
        }
        (*env)->ReleaseByteArrayElements(env, buffer, buf, 0);
        return buffer;
    }

    char* y = media->vp[0].ptr;
    if (width == media->vp[0].stride) {
        intptr_t bytes = width * h;
        memcpy(pch, y, bytes);
    } else {
        char* p = pch;
        for (int i = 0; i < h; ++i) {
            memcpy(p, y, w);
            p += width;
            y += media->vp[0].stride;
        }
    }
    pch += width * height;

    h /= 2;
    if (csp == csp_nv12) {
        if (color_space == csp_nv21) {
            for (int i = 0; i < h; ++i) {
                char* uv = pch + width * i;
                char* vu = media->vp[1].ptr + media->vp[1].stride * i;
                for (int j = 0; j < w; j += 2) {
                    uv[0] = vu[1];
                    uv[1] = vu[0];
                    vu += 2;
                    uv += 2;
                }
            }
        } else {
            for (int i = 0; i < h; ++i) {
                char* uv = pch + width * i;
                char* u = media->vp[1].ptr + media->vp[1].stride * i;
                char* v = media->vp[2].ptr + media->vp[2].stride * i;
                for (int j = 0; j < w; j += 2) {
                    uv[0] = u[0];
                    uv[1] = v[0];
                    u += 1;
                    v += 1;
                }
            }
        }
    } else if (csp == csp_nv21) {
        if (color_space == csp_nv12) {
            for (int i = 0; i < h; ++i) {
                char* vu = pch + width * i;
                char* uv = media->vp[1].ptr + media->vp[1].stride * i;
                for (int j = 0; j < w; j += 2) {
                    vu[0] = uv[1];
                    vu[1] = uv[0];
                    vu += 2;
                    uv += 2;
                }
            }
        } else {
            for (int i = 0; i < h; ++i) {
                char* vu = pch + width * i;
                char* u = media->vp[1].ptr + media->vp[1].stride * i;
                char* v = media->vp[2].ptr + media->vp[2].stride * i;
                for (int j = 0; j < w; j += 2) {
                    vu[0] = v[0];
                    vu[1] = u[0];
                    u += 1;
                    v += 1;
                }
            }
        }
    } else if (csp == csp_i420) {
        width /= 2;
        height /= 2;
        intptr_t offset = height * width;
        for (int i = 0; i < h; ++i) {
            char* u = pch + width * i;
            char* v = u + offset;
            char* vu = media->vp[1].ptr + media->vp[1].stride * i;
            for (int j = 0; j < w; j += 2) {
                u[0] = vu[1];
                v[0] = vu[0];
                vu += 2;
                u += 1;
                v += 1;
            }
        }
    }

    (*env)->ReleaseByteArrayElements(env, buffer, buf, 0);
    return buffer;
}

static jbyteArray jni_pixel_pull(JNIEnv* env, jobject obj, jlong something, jint width, jint height, jint csp, jlongArray meta)
{
    jbyteArray ba = NULL;
    struct my_buffer* mbuf = NULL;
    jlong* buf = (*env)->GetLongArrayElements(env, meta, NULL);
    buf[0] = 0;

    my_handle* handle = (my_handle *) (intptr_t) something;
    java_h264_t* ctx = (java_h264_t *) handle_get(handle);
    if (ctx == NULL) {
        buf[0] = -1;
        goto LABEL;
    }

    lock(&ctx->lck);
    if (!list_empty(&ctx->head[1])) {
        ctx->nr[1] -= 1;
        struct list_head* ent = ctx->head[1].next;
        list_del(ent);
        mbuf = list_entry(ent, struct my_buffer, head);
    }
    unlock(&ctx->lck);
    handle_put(handle);
    if (mbuf != NULL) {
        media_buffer* media = (media_buffer *) mbuf->ptr[0];
        buf[0] = media->angle;
        buf[1] = media->pts;

        int64_t* feature = android_feature_address();
        if (feature[bug_mask] & bug_nv12_nv21) {
            if (csp == csp_nv12) {
                csp = csp_nv21;
            } else if (csp == csp_nv21) {
                csp = csp_nv12;
            }
        }

        video_format* fmt = to_video_format(media->pptr_cc);
        uint32_t color_space = color_space_normalize(fmt->pixel->csp);
        if (csp == color_space && width == media->vp[0].stride && height == media->vp[0].height) {
            java_buffer_t* jbuf = (java_buffer_t *) (mbuf->ptr[1] + mbuf->length);
            ba = (jbyteArray) (*env)->NewLocalRef(env, jbuf->pixel);
        } else {
            ba = copy_pixels(env, media, width, height, csp);
        }
        mbuf->mop->free(mbuf);
    }
LABEL:
    (*env)->ReleaseLongArrayElements(env, meta, buf, 0);
    return ba;
}

static int32_t analyse_nalu(char* nalu, intptr_t* nbp)
{
    intptr_t bytes = *nbp;
#if !defined(NDEBUG)
    intptr_t check = 1;
#else
    intptr_t check = 0;
#endif
    char annexb[] = {0, 0, 1};
    int32_t type = 0, pps = 0, sei = 0;

    while (bytes > 0) {
        my_assert(nalu[0] == 0);
        my_assert(nalu[1] == 0);

        intptr_t annexb_nb = 3;
        if (nalu[2] == 0) {
            annexb_nb = 4;
        }
        nalu += annexb_nb;
        bytes -= annexb_nb;

        int nalu_type = (nalu[0] & 0x1f);
        if (nalu_type == 7) {
            my_assert(type == 0);
            type = nalu_type;
        } else if (type == 7) {
            if (nalu_type == 6) {
                sei = 1;
            } else if (nalu_type == 8) {
                pps = 1;
            } else {
                *nbp -= (bytes + annexb_nb);
                if (nalu[-4] == 0) {
                    *nbp -= 1;
                }
                break;
            }
        } else {
            my_assert(nalu_type != 6);
            my_assert(nalu_type != 8);
            type = nalu_type;
        }

        if (type == 7 || check == 1) {
            char* pch = memmem(nalu, bytes, annexb, 3);
            if (pch != NULL) {
                my_assert(type == 7);
                bytes -= (pch - nalu);
                nalu = pch;
                continue;
            }
        }
        break;
    }

    if (type == 7) {
        my_assert(pps == 1);
        return video_type_sps;
    }

    if (type == 5) {
        return video_type_idr;
    }

    if (type != 1) {
        logmsg("unexpected nalu_type %d", type);
        return -1;
    }

    nalu -= 3;
    bytes += 3;
    if (nalu[-1] == 0) {
        nalu -= 1;
        bytes += 1;
    }
    return none_dir_frametype(nalu, bytes);
}

static intptr_t add_h264_frame(java_h264_t* ctx, uint32_t angle, uint32_t pts, struct my_buffer** mbufp)
{
    struct my_buffer* mbuf = *mbufp;
    intptr_t length = mbuf->length;

    media_buffer* media = (media_buffer *) mbuf->ptr[0];
    media->frametype = analyse_nalu(mbuf->ptr[1], &mbuf->length);
    if (media->frametype == (uint32_t) -1) {
        mbuf->mop->free(mbuf);
        return -1;
    }

    length -= mbuf->length;
    if (__builtin_expect(length > 0, 0)) {
        struct my_buffer* mbuf2 = NULL;
        if (ctx->sps == 0) {
            mbuf2 = mbuf_alloc_2(sizeof(media_buffer) + length);
            if (mbuf2 == NULL) {
                length = 0;
            } else {
                mbuf2->length -= sizeof(media_buffer);
                mbuf2->ptr[1] += sizeof(media_buffer);
                memcpy(mbuf2->ptr[1], mbuf->ptr[1] + mbuf->length, length);
            }
        } else {
            mbuf->ptr[1] += mbuf->length;
            mbuf->length = length;
            mbuf2 = mbuf;
            mbuf = NULL;
        }
        *mbufp = mbuf2;
    }

    if (__builtin_expect(mbuf != NULL, 1)) {
        if (media->frametype == video_type_sps) {
            ctx->sps = 1;
            stream_begin(mbuf);
        }

        memset(&media->vp, 0, sizeof(media->vp));
        media->angle = angle;
        media->fragment[0] = 0;
        media->fragment[1] = 1;
        media->pptr_cc = ctx->out;
        media->iden = ctx->id;
        media->seq = ctx->seq++;
        media->pts = pts;
        media->vp[0].stride = (uint32_t) mbuf->length;
        media->vp[0].height = 1;
        media->vp[0].ptr = mbuf->ptr[1];

        lock(&ctx->lck);
        list_add_tail(&mbuf->head, &ctx->head[0]);
        ctx->nr[0] += 1;
        unlock(&ctx->lck);
    }

    return length;
}

static jint jni_h264_push(JNIEnv* env, jobject obj, jlong something, jlong angle, jlong pts, jbyteArray pixel)
{
    my_handle* handle = (my_handle *) (intptr_t) something;
    java_h264_t* ctx = (java_h264_t *) handle_get(handle);
    if (ctx == NULL) {
        return -1;
    }

    jint nr = (*env)->GetArrayLength(env, pixel);
    struct my_buffer* mbuf = mbuf_alloc_2(sizeof(media_buffer) + nr);
    if (mbuf == NULL) {
        goto LABEL;
    }

    mbuf->length -= sizeof(media_buffer);
    mbuf->ptr[1] += sizeof(media_buffer);
    (*env)->GetByteArrayRegion(env, pixel, 0, nr, (jbyte *) mbuf->ptr[1]);

    intptr_t left = mbuf->length;
    struct my_buffer** mbufp = &mbuf;
    do {
        left = add_h264_frame(ctx, (uint32_t) angle, (uint32_t) pts, mbufp);
    } while (left > 0);
LABEL:
    handle_put(handle);
    return 0;
}

static void jni_handle_dereference(JNIEnv* env, jclass clazz, jlong handle)
{
    handle_release((my_handle *) (intptr_t) handle);
}

static JNINativeMethod codec_methods[] = {
    {
        .name = "handle_dereference",
        .signature = "(J)V",
        .fnPtr = jni_handle_dereference
    },

    {
        .name = "pixel_pull",
        .signature = "(JIII[J)[B",
        .fnPtr = jni_pixel_pull
    },

    {
        .name = "h264_push",
        .signature = "(JJJ[B)I",
        .fnPtr = jni_h264_push
    }
};

__attribute__((constructor)) static void media_register()
{
    static struct java_mehods media_entry = methods_entry(libmedia, codec, lib_open, NULL, runtime_check_encoder);
    java_method_register(&media_entry);
}
#endif
