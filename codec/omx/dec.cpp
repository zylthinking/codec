
#if defined(__ANDROID__)
#include <binder/ProcessState.h>
#include <media/stagefright/OMXClient.h>
#include <media/stagefright/OMXCodec.h>
using namespace android;

#include "mpu.h"
#include "mbuf.h"
#include "media_buffer.h"
#include "my_errno.h"
#include "mem.h"
#include "media_source.h"
#include "android.h"
#include "fmt.h"
#include "lock.h"
#include "vendor.h"
#include "omxil.h"
#include <string.h>

#include <pthread.h>
#include <sys/syscall.h>
#define futex(p1, p2, p3, p4, p5, p6) syscall(__NR_futex, p1, p2, p3, p4, p5, p6)

static const char* omx_ti = "OMX.TI.DUCATI1.VIDEO.DECODER";
extern "C" video_format* ti_video_format(int32_t csp, uintptr_t width, uintptr_t height, vendor_ops_t** ops_ptr);

static const char* omx_qcom = "OMX.qcom.video.decoder.avc";
extern "C" video_format* qcom_video_format(int32_t csp, uintptr_t width, uintptr_t height, vendor_ops_t** ops_ptr);

static const char* omx_mtk = "OMX.MTK.VIDEO.DECODER.AVC";
extern "C" video_format* mtk_video_format(int32_t csp, uintptr_t width, uintptr_t height, vendor_ops_t** ops_ptr);

static const char* omx_sec = "OMX.SEC.avc.dec";
extern "C" video_format* sec_video_format(int32_t csp, uintptr_t width, uintptr_t height, vendor_ops_t** ops_ptr);

typedef struct {
    struct my_buffer* sps;
    OMXClient omx;
    const char* vendor;
    sp<media_source> input;
    sp<MediaSource> output;
    intptr_t refs;

    uint64_t seq;
    media_iden_t id;
    int angle;

    lock_t lck;
    int32_t nr[3];
    struct list_head head;

    video_format* outfmt;
    vendor_ops_t* vendor_ops;
    uintptr_t hold[2];
    MediaBuffer* buffers[128];
} omx_context_t;

static void omx_context_free(omx_context_t* ctx)
{
    intptr_t n = __sync_sub_and_fetch(&ctx->refs, 1);
    // if ctx->input == NULL, no reading will being called at all.
    // if ctx->input != NULL, the reading may blocks, input->stop interrupt this.
    // And stop allows to be called multi-times; so no problem here.
    if (ctx->input != NULL) {
        ctx->input->stop();
    }

    ctx->angle = 0;
    futex(&ctx->angle, FUTEX_WAKE, 1, NULL, NULL, 0);

    if (n != 0) {
        my_assert(n > 0);
        return;
    }
    ctx->omx.disconnect();
    free_buffer(&ctx->head);
    delete ctx;
}

static video_format* video_format_from_oxmcsp(const char* vendor, uintptr_t width, uintptr_t height, int32_t omx_color_space, vendor_ops_t** ops)
{
    video_format* fmt = NULL;
    if (omx_color_space > OMX_COLOR_FormatVendorStartUnused) {
        if (vendor == omx_qcom) {
            fmt = qcom_video_format(omx_color_space, width, height, ops);
        } else if (vendor == omx_mtk) {
            fmt = mtk_video_format(omx_color_space, width, height, ops);
        } else if (vendor == omx_ti) {
            fmt = ti_video_format(omx_color_space, width, height, ops);
        }
        return fmt;
    }

    if (vendor == omx_sec) {
        fmt = sec_video_format(omx_color_space, width, height, ops);
    }

    if (fmt != NULL) {
        return fmt;
    }

    if (omx_color_space == OMX_COLOR_FormatYUV420Planar) {
        fmt = video_format_get(codec_pixel, csp_i420, width, height);
    } else if (omx_color_space == OMX_COLOR_FormatYUV420SemiPlanar) {
        fmt = video_format_get(codec_pixel, csp_nv12, width, height);
    } else {
        logmsg("color space %d does not surpport yet\n", omx_color_space);
    }
    return fmt;
}

static video_format* output_format(omx_context_t* ctx, uintptr_t width, uintptr_t height, int32_t omx_color_space)
{
    static vendor_ops_t generic_ops = {
        generic_panel_copy,
        NULL
    };
    ctx->vendor_ops = &generic_ops;

    video_format* fmt = video_format_from_oxmcsp(ctx->vendor, width, height, omx_color_space, &ctx->vendor_ops);
    return fmt;
}

static intptr_t stream_header_save(media_buffer* media, char* sps, intptr_t sps_bytes, char* pps, intptr_t pps_bytes)
{
    intptr_t n = sps_bytes + pps_bytes + 8;
    struct my_buffer* header = mbuf_alloc_2(sizeof(media_buffer) + n);
    if (header == NULL) {
        return -1;
    }
    header->ptr[1] += sizeof(media_buffer);
    header->length -= sizeof(media_buffer);;
    media_buffer* media2 = (media_buffer *) header->ptr[0];
    *media2 = *media;
    media2->vp[0].ptr = header->ptr[1];
    media2->vp[0].stride = (uint32_t) header->length;

    char* cur = media2->vp[0].ptr;
    *(cur++) = 0;
    *(cur++) = 0;
    *(cur++) = 0;
    *(cur++) = 1;
    memcpy(cur, sps, sps_bytes);
    cur += sps_bytes;

    *(cur++) = 0;
    *(cur++) = 0;
    *(cur++) = 0;
    *(cur++) = 1;
    memcpy(cur, pps, pps_bytes);

    stream_begin(header);
    header->mop->free(header);
    return 0;
}

static intptr_t sps_pps_to_avcc(media_buffer* media, char* avcc, char*& sps_pps, uintptr_t& bytes)
{
    char* sps = NULL;
    intptr_t sps_bytes = 0;
    char* pps = NULL;
    intptr_t pps_bytes = 0;

    char code[3] = {0, 0, 1};
    while (sps == NULL || pps == NULL) {
        my_assert(sps_pps[0] == 0 && sps_pps[1] == 0);
        if (sps_pps[2] == 0) {
            my_assert(sps_pps[3] == 1);
            sps_pps += 4;
            bytes -= 4;
        } else {
            my_assert(sps_pps[2] == 1);
            sps_pps += 3;
            bytes -= 3;
        }

        char* end = (char *) memmem(sps_pps, bytes, code, 3);
        if (end == NULL) {
            end = sps_pps + bytes;
        } else if (end[-1] == 0) {
            end -= 1;
        }

        intptr_t type = sps_pps[0] & 0x1f;
        intptr_t length = (intptr_t) (end - sps_pps);
        my_assert(bytes >= length);

        if (type == 7) {
            sps = sps_pps;
            sps_bytes = length;
        } else if (type == 8) {
            pps = sps_pps;
            pps_bytes = length;
        } else if (sps != NULL && pps != NULL) {
            break;
        }

        sps_pps = end;
        bytes -= length;
    }

    if (sps == NULL || pps == NULL) {
        my_assert(0);
        return -1;
    }

    if (-1 == stream_header_save(media, sps, sps_bytes, pps, pps_bytes)) {
        return -1;
    }

    // ISO/IEC 14496-15
    intptr_t idx = 0;
    avcc[idx++] = 1;
    avcc[idx++] = sps[1];
    avcc[idx++] = sps[2];
    avcc[idx++] = sps[3];
    mark("profile %d level %d", sps[1], sps[3]);
    avcc[idx++] = 0xff;
    avcc[idx++] = 0xe1;
    avcc[idx++] = (char) (sps_bytes >> 8);
    avcc[idx++] = (char) sps_bytes;
    memcpy(&avcc[idx], sps, sps_bytes);
    idx += sps_bytes;

    avcc[idx++] = 1;
    avcc[idx++] = (char) (pps_bytes >> 8);
    avcc[idx++] = (char) pps_bytes;
    memcpy(&avcc[idx], pps, pps_bytes);
    idx += pps_bytes;

    return idx;
}

static const char* vendor_of(const char* vendor)
{
    if (0 == strcmp(vendor, omx_mtk)) {
        vendor = omx_mtk;
    } else if (0 == strcmp(vendor, omx_qcom)) {
        vendor = omx_qcom;
    } else if (0 == strcmp(vendor, omx_ti)) {
        vendor = omx_ti;
    } else if (0 == strcmp(vendor, omx_sec)) {
        vendor = omx_sec;
    } else {
        vendor = NULL;
    }
    return vendor;
}

static intptr_t omx_initialize(omx_context_t* ctx, struct my_buffer* mbuf)
{
    media_buffer* media = (media_buffer *) mbuf->ptr[0];
    video_format* fmt = to_video_format(media->pptr_cc);

    sp<MetaData> sp_meta = new (std::nothrow) MetaData;
    if (sp_meta == NULL) {
        return -1;
    }

    char avcc[40960];
    char* sps_pps = mbuf->ptr[1];
    uintptr_t nb = mbuf->length;
    intptr_t avcc_bytes = sps_pps_to_avcc(media, avcc, sps_pps, nb);
    my_assert2(sizeof(avcc) > avcc_bytes, "sps_pps too long");

    sp_meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_AVC);
    sp_meta->setInt32(kKeyWidth, fmt->pixel->size->width);
    sp_meta->setInt32(kKeyHeight, fmt->pixel->size->height);
    // avcc should be uneceesary because we will feed spspps in the steam
    // mtk force use avcc and no spspps in the stream
    sp_meta->setData(kKeyAVCC, kTypeAVCC, avcc, avcc_bytes);

    intptr_t size = roundup(fmt->pixel->size->width, 16) * fmt->pixel->size->height * 3;
    media_source* source = new (std::nothrow) media_source(sp_meta);
    if (source == NULL || -1 == source->initialize(size)) {
        delete source;
        return -1;
    }
    ctx->input = source;

    uint32_t flag = OMXCodec::kClientNeedsFramebuffer;
    sp<MediaSource> sp_out_port = OMXCodec::Create(ctx->omx.interface(),
                                                   sp_meta,
                                                   false,
                                                   ctx->input,
                                                   NULL,
                                                   OMXCodec::kHardwareCodecsOnly);
    if (sp_out_port == NULL) {
        logmsg("OMXCodec::Create failed\n");
        return -1;
    }

    int32_t csp;
    sp<MetaData> meta = sp_out_port->getFormat();
    meta->findInt32(kKeyColorFormat, &csp);

    meta->findCString(kKeyDecoderComponent, &ctx->vendor);
    ctx->vendor = vendor_of(ctx->vendor);
    ctx->outfmt = output_format(ctx, fmt->pixel->size->width, fmt->pixel->size->height, csp);
    if (ctx->outfmt == NULL) {
        return -1;
    }

    if (ctx->vendor == omx_qcom) {
        OMXCodec* codec = (OMXCodec *) sp_out_port.get();
        qcom_low_lantency(ctx->omx.interface().get(), codec->nodeid());
    }

    status_t stat = sp_out_port->start();
    if (OK != stat) {
        logmsg("omx start status = %d\n", stat);
        return -1;
    }

    ctx->output = sp_out_port;
    mbuf->ptr[1] = sps_pps;
    mbuf->length = nb;
    media->frametype = video_type_idr;
    media->vp[0].stride = (uint32_t) mbuf->length;
    media->vp[0].ptr = mbuf->ptr[1];
    return 0;
}

static sp<MediaSource> wait_sps_packet(omx_context_t* ctx)
{
    while (ctx->angle == -1) {
        intptr_t n = __sync_add_and_fetch(&ctx->refs, 1);
        if (n == 3) {
            futex(&ctx->angle, FUTEX_WAIT, -1, NULL, NULL, 0);
        }

        if (1 == __sync_sub_and_fetch(&ctx->refs, 1)) {
            return NULL;
        }
    }
    return ctx->output;
}

static uintptr_t chunk_size(omx_context_t* ctx, sp<MetaData> meta, MediaBuffer* buffer,
                            panel_t chunk0[3], panel_t chunk1[3])
{
    video_format* fmt = ctx->outfmt;
    int32_t w0 = fmt->pixel->size->width;
    int32_t h0 = fmt->pixel->size->height;
    int32_t nb0 = 0;

    int32_t w1, h1, nb1 = 0;
    meta->findInt32(kKeyWidth, &w1);
    meta->findInt32(kKeyHeight, &h1);

    panel_t chunk[3], *chunk2 = NULL;
    int32_t nb2 = 0;
    if (ctx->vendor_ops->size_fixup != NULL) {
        ctx->vendor_ops->size_fixup(w1, h1, w0, h0, chunk);
        chunk2 = &chunk[0];
    }

    for (int i = 0; i < fmt->pixel->panels; ++i) {
        chunk1[i].width = panel_width(i, w1, fmt->pixel->csp);
        chunk1[i].width = pixels_to_bytes(i, chunk1[i].width, fmt->pixel->csp);
        chunk1[i].height = panel_height(i, h1, fmt->pixel->csp);
        chunk1[i].bytes = chunk1[i].width * chunk1[i].height;
        nb1 += chunk1[i].bytes;

        if (chunk2 != NULL) {
            nb2 += chunk[i].bytes;
        }

        chunk0[i].width = panel_width(i, w0, fmt->pixel->csp);
        chunk0[i].width = pixels_to_bytes(i, chunk0[i].width, fmt->pixel->csp);
        chunk0[i].height = panel_height(i, h0, fmt->pixel->csp);
        chunk0[i].bytes = chunk0[i].width * chunk0[i].height;
        nb0 += chunk0[i].bytes;
    }

    int32_t nb = (int32_t) buffer->range_length();
    if (chunk2 != NULL) {
        if (nb < nb2) {
            my_assert2(0, "kKeyWidth %d, kKeyHeight %d, fixed width: %d, fixed height: %d, %d:%d",
                       w1, h1, (int32_t) chunk[0].width, (int32_t) chunk[0].height, nb, nb2);
        }
        memcpy(chunk1, chunk2, sizeof(chunk));
    } else if (nb < nb1 && 0) {
        trace_change2(systid(), "kKeyWidth %d, kKeyHeight %d, buffer bytes %d less than %d", w1, h1, nb, nb1);
    }

#if 0
    int32_t l, r, t, b;
    meta->findRect(kKeyCropRect, &l, &t, &r, &b);
    mark("%p nb %d(%d) nb1 %d nb2 %d, left %d right %d top %d bottom %d in %d:%d",
         buffer->data(), nb, (int32_t) buffer->range_offset(),
         nb1, nb2, l, r, t, b, w1, h1);
#endif
    return nb0;
}

static struct my_buffer* mbuf_from_frame(omx_context_t* ctx, sp<MediaSource> port, MediaBuffer* buffer)
{
    panel_t chunk0[3] = {0}, chunk1[3] = {0};
    sp<MetaData> meta = port->getFormat();
    intptr_t bytes = chunk_size(ctx, meta, buffer, chunk0, chunk1);
    if (bytes == -1) {
        return NULL;
    }

    int64_t pts;
    buffer->meta_data()->findInt64(kKeyTime, &pts);

    struct my_buffer* mbuf = mbuf_alloc_2(sizeof(media_buffer) + bytes);
    if (mbuf == NULL) {
        return NULL;
    }
    mbuf->ptr[1] = mbuf->ptr[0] + sizeof(media_buffer);
    mbuf->length -= sizeof(media_buffer);

    media_buffer* media1 = (media_buffer *) mbuf->ptr[0];
    memset(media1->vp, 0, sizeof(media1->vp));
    media1->frametype = 0;
    media1->fragment[0] = 0;
    media1->fragment[1] = 1;

    media1->pptr_cc = &ctx->outfmt->cc;
    media1->seq = ctx->seq++;
    media1->iden = ctx->id;
    media1->pts = pts / 1000;
    media1->angle = pts % 1000;

    char* pch = (char *) buffer->data();
    pch += buffer->range_offset();
    int32_t nb = (int32_t) buffer->range_length();
    char* addr = mbuf->ptr[1];
    for (int i = 0; i < ctx->outfmt->pixel->panels; ++i) {
        media1->vp[i].stride = (uint32_t) chunk0[i].width;
        media1->vp[i].height = (uint32_t) chunk0[i].height;
        media1->vp[i].ptr = addr;
        addr += chunk0[i].bytes;
    }
    ctx->vendor_ops->panel_copy(media1, pch, nb, chunk0, chunk1);
    return mbuf;
}

static void* omx_pull_thread(void* args)
{
    mark("thread start");
    intptr_t n = 0;
    omx_context_t* ctx = (omx_context_t *) args;

    sp<MediaSource> port = wait_sps_packet(ctx);
    if (port == NULL) {
        goto LABEL;
    }

    do {
        n = __sync_add_and_fetch(&ctx->refs, 1);
        my_assert(n >= 2);
        if (n == 2) {
            __sync_sub_and_fetch(&ctx->refs, 1);
            break;
        }

        MediaBuffer* buffer = NULL;
        status_t status = port->read(&buffer);
        if (status == INFO_FORMAT_CHANGED) {
            if (buffer != NULL) {
                buffer->release();
            }
            __sync_sub_and_fetch(&ctx->refs, 1);
            continue;
        }

        if (status != OK) {
            __sync_sub_and_fetch(&ctx->refs, 1);
            if (status == -ETIMEDOUT) {
                continue;
            }
            logmsg("omx failed status = %d\n", status);
            break;
        }

        size_t bytes = buffer->range_length();
        if (bytes != 0) {
            struct my_buffer* mbuf = mbuf_from_frame(ctx, port, buffer);
            if (mbuf != NULL) {
                lock(&ctx->lck);
                ++ctx->nr[0];
                ++ctx->nr[2];
                list_add_tail(&mbuf->head, &ctx->head);
                unlock(&ctx->lck);
            }
        }

        if (ctx->hold[1] < ctx->hold[0]) {
            ctx->buffers[ctx->hold[1]] = buffer;
            ++ctx->hold[1];
        } else {
            buffer->release();
        }

        n = __sync_sub_and_fetch(&ctx->refs, 1);
    } while (n > 1);

    for (int i = 0; i < ctx->hold[1]; ++i) {
        MediaBuffer* buffer = ctx->buffers[i];
        buffer->release();
    }
    port->stop();
LABEL:
    omx_context_free(ctx);
    mark("thread exit");
    return NULL;
}

static void* omx_open(fourcc** in, fourcc** out)
{
    int64_t* feature = android_feature_address();
    ProcessState::self()->startThreadPool();
    omx_context_t* ctx = new (std::nothrow) omx_context_t;
    if (ctx == NULL) {
        return NULL;
    }

    ctx->sps = NULL;
    ctx->id = media_id_unkown;
    ctx->seq = 0;
    ctx->angle = -1;
    ctx->vendor = NULL;
    ctx->outfmt = NULL;
    ctx->input = NULL;
    ctx->output = NULL;
    ctx->refs = 2;
    ctx->nr[0] = ctx->nr[1] = ctx->nr[2] = 0;
    ctx->lck = lock_val;
    ctx->vendor_ops = NULL;
    ctx->hold[0] = my_min(elements(ctx->buffers), feature[omx_usurp]);
    ctx->hold[1] = 0;
    memset(ctx->buffers, 0, sizeof(ctx->buffers));

    INIT_LIST_HEAD(&ctx->head);

    if (OK != ctx->omx.connect()) {
        delete ctx;
        return NULL;
    }

    pthread_t tid;
    if (0 != pthread_create(&tid, NULL, omx_pull_thread, ctx)) {
        ctx->omx.disconnect();
        delete ctx;
        ctx = NULL;
    } else {
        pthread_detach(tid);
    }
    return (void *) ctx;
}

static int32_t omx_write(void* any, struct my_buffer* mbuf, struct list_head* headp, int32_t* delay)
{
    omx_context_t* ctx = (omx_context_t *) any;
    if (__builtin_expect(ctx->angle == -1, 0)) {
        media_buffer* media = NULL;
        if (ctx->sps == NULL) {
            media = (media_buffer *) mbuf->ptr[0];
            if (video_type_sps != media->frametype) {
                mbuf->mop->free(mbuf);
                return 0;
            }
            ctx->sps = mbuf;
            mbuf = NULL;
        }
        media = (media_buffer *) ctx->sps->ptr[0];

        if (0 == omx_initialize(ctx, ctx->sps)) {
            ctx->id = media->iden;
            ctx->angle = media->angle;
            futex(&ctx->angle, FUTEX_WAKE, 1, NULL, NULL, 0);
        } else {
            logmsg("omx_initialize failed\n");
            if (mbuf != NULL) {
                mbuf->mop->free(mbuf);
            }
            return 0;
        }

        if (ctx->sps->length == 0) {
            ctx->sps->mop->free(ctx->sps);
            ctx->sps = NULL;
            if (mbuf == NULL) {
                return 0;
            }
        }

        if (mbuf == NULL) {
            mbuf = ctx->sps;
            ctx->sps = NULL;
        } else {
            if (-1 == ctx->input->add_buffer(ctx->sps)) {
                ctx->sps->mop->free(ctx->sps);
            }
            ctx->sps = NULL;
        }
    }

    intptr_t total_frames = ctx->input->add_buffer(mbuf);
    if (-1 == total_frames) {
        mbuf->mop->free(mbuf);
    } else {
        ctx->nr[1] = total_frames;
    }

    int32_t nr = 0;
    lock(&ctx->lck);
#if 1
    while (ctx->nr[0] > 0) {
        struct list_head* ent = ctx->head.next;
        my_assert(!list_empty(ent));

        list_del(ent);
        list_add_tail(ent, headp);
        ctx->nr[0] -= 1;
        nr += 1;
        if (ctx->nr[0] <= 4) {
            break;
        }
    }
#else
    list_join(headp, &ctx->head);
    list_del_init(&ctx->head);
    nr = ctx->nr[0];
    ctx->nr[0] = 0;
#endif

    *delay = ctx->nr[1] - ctx->nr[2] + ctx->nr[0];
    //logmsg("ctx->nr[0] = %d, delay = %d", (int) ctx->nr[0], *delay);
    unlock(&ctx->lck);
    return nr;
}

static void omx_close(void* any)
{
    omx_context_t* ctx = (omx_context_t *) any;
    omx_context_free(ctx);
}

static mpu_operation omx_ops = {
    omx_open,
    omx_write,
    omx_close
};

static media_process_unit omx_h264_dec = {
    &h264_0x0.cc,
    &raw_0x0.cc,
    &omx_ops,
    1,
    mpu_decoder,
    "omx_h264_dec"
};

static intptr_t decoder_probe(OMXClient& omx, const char*& vendor)
{
    sp<MetaData> sp_meta = new (std::nothrow) MetaData;
    if (sp_meta == NULL) {
        return -1;
    }
    sp_meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_AVC);
    sp_meta->setInt32(kKeyWidth, 640);
    sp_meta->setInt32(kKeyHeight, 480);

    media_source* source = new (std::nothrow) media_source(sp_meta);
    if (source == NULL) {
        delete source;
        return -1;
    }
    sp<media_source> input = source;

    sp<MediaSource> sp_out_port = OMXCodec::Create(omx.interface(),
                                                   sp_meta,
                                                   false,
                                                   input,
                                                   NULL,
                                                   OMXCodec::kHardwareCodecsOnly);
    if (sp_out_port == NULL) {
        return -1;
    }

    int32_t csp;
    sp<MetaData> meta = sp_out_port->getFormat();
    meta->findInt32(kKeyColorFormat, &csp);

    meta->findCString(kKeyDecoderComponent, &vendor);
    const char* pch = vendor_of(vendor);
    video_format* fmt = video_format_from_oxmcsp(pch, 640, 480, csp, NULL);
    if (fmt != NULL) {
        logmsg("hardware decoder enabled %s, 0x%x\n", vendor, csp);
        return 0;
    }
    logmsg("hardware decoder disabled %s, 0x%x\n", vendor, csp);
    return -1;
}

capi media_process_unit* mpu_get_decoder()
{
    const char* vendor = NULL;
    OMXClient omx;
    if (OK != omx.connect()) {
        return NULL;
    }
    int n = decoder_probe(omx, vendor);
    omx.disconnect();

    if (n == -1) {
        return NULL;
    }
    return &omx_h264_dec;
}
#endif
