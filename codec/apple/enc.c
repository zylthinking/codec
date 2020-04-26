
#if defined(__APPLE__)
#include <VideoToolbox/VideoToolbox.h>
#include "ios.h"
#include "mpu.h"
#include "mem.h"
#include "mbuf.h"
#include "my_handle.h"
#include "lock.h"
#include "nalu.h"

typedef struct {
    VTCompressionSessionRef session;
    my_handle* self;
    fourcc** in;
    fourcc** out;
    lock_t lck;

    uint32_t seq;
    uint32_t frames;
    media_iden_t iden;
    uintptr_t reflost;
    struct list_head head;
    struct my_buffer* sps;
} apple_wrapper_t;

static uint32_t nalu_type(char* pch, uint32_t nb)
{
    uint32_t frametype = (pch[4] & 0x1f);
    if (frametype == 6) {
        return video_type_sei;
    }

    if (frametype == 5) {
        return video_type_idr;
    }

    if (frametype != 1) {
        logmsg("unexpected nalu_type %d", frametype);
        return video_type_unkown;
    }
    return none_dir_frametype(pch, nb);
}

static uint32_t block_check_nalu_type(CMBlockBufferRef block)
{
    char buffer[8], *pch = NULL;
    uint32_t frametype = video_type_unkown;
    size_t total = CMBlockBufferGetDataLength(block);

    while (total > 0) {
        uint32_t nb = (uint32_t) my_min(8, total);
        OSStatus status = CMBlockBufferAccessDataBytes(block, 0, nb, buffer, &pch);
        my_assert(status == kCMBlockBufferNoErr);

        nb = *(uint32_t *) pch;
        *(uint32_t *) pch = 0x01000000;
        nb = ntohl(nb);
        total -= (nb + 4);

        frametype = nalu_type(pch, nb + 4);
        if (reference_able(frametype)) {
            break;
        }
    }
    return frametype;
}

static void skip_sei(media_buffer* media)
{
    char* pch = media->vp[0].ptr;
    uint32_t nb = *(uint32_t *) pch;
    nb = ntohl(nb) + 4;

    while ((media->vp[0].stride >= nb) && (6 == (pch[4] & 0x1f))) {
        *(uint32_t *) pch = 0x01000000;
        media->vp[0].ptr = pch + nb;
        media->vp[0].stride -= nb;

        pch = media->vp[0].ptr;
        nb = *(uint32_t *) pch;
        nb = ntohl(nb) + 4;
    }

    if (__builtin_expect(media->vp[0].stride < nb, 0)) {
        my_assert2(0, "%s", "nalu is not correct\n");
    }
}

static uint32_t mbuf_check_nalu_type(struct my_buffer* mbuf)
{
    media_buffer* media = (media_buffer *) mbuf->ptr[0];
    skip_sei(media);
    if (media->vp[0].stride == 0) {
        return video_type_sei;
    }

    char* pch = media->vp[0].ptr;
    uint32_t total = media->vp[0].stride;
    uint32_t frametype = video_type_unkown;

    while (total > 0) {
        uint32_t nb = *(uint32_t *) pch;
        *(uint32_t *) pch = 0x01000000;
        nb = ntohl(nb) + 4;

        uint32_t type = nalu_type(pch, nb);
        pch += nb;
        total -= nb;

        if (type == video_type_sei) {
            logmsg("found sei\n");
            continue;
        } else if (type == video_type_unkown) {
            continue;
        }

        my_assert2(frametype == video_type_unkown || type == frametype,
                   "old frametype %u does not match %u", frametype, type);
        frametype = type;
    }
    return frametype;
}

static intptr_t depends_by_others(CMSampleBufferRef h264)
{
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(h264, false);
    if(attachments == NULL) {
        return 0;
    }
    CFDictionaryRef attachment = (CFDictionaryRef) CFArrayGetValueAtIndex(attachments, 0);
    CFBooleanRef depends_by_others = (CFBooleanRef) CFDictionaryGetValue(attachment, kCMSampleAttachmentKey_IsDependedOnByOthers);
    return (depends_by_others == kCFBooleanTrue);
}

static struct my_buffer* init_sps(CMSampleBufferRef h264, apple_wrapper_t* wrapper)
{
    CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(h264);
    size_t sps_bytes, pps_bytes;
    const uint8_t *sps, *pps;

    CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, &sps, &sps_bytes, NULL, NULL);
    CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 1, &pps, &pps_bytes, NULL, NULL);
    my_assert(sps_bytes + pps_bytes + 8 < wrapper->sps->length);
    wrapper->sps->length = sps_bytes + pps_bytes + 8;

    char* pch = wrapper->sps->ptr[1];
    *(uint32_t *) pch = 0x01000000;
    pch += 4;
    memcpy(pch, sps, sps_bytes);
    pch += sps_bytes;

    *(uint32_t *) pch = 0x01000000;
    pch += 4;
    memcpy(pch, pps, pps_bytes);
    pch += pps_bytes;

    media_buffer* media = (media_buffer *) wrapper->sps->ptr[0];
    memset(media->vp, 0, sizeof(media->vp));
    media->pptr_cc = wrapper->out;
    media->iden = wrapper->iden;
    media->fragment[0] = 0;
    media->fragment[1] = 1;
    media->frametype = video_type_sps;
    media->angle = 0;
    media->seq = (++wrapper->seq);
    media->pts = 0;

    media->vp[0].ptr = wrapper->sps->ptr[1];
    media->vp[0].stride = (uint32_t) wrapper->sps->length;
    media->vp[0].height = 1;

    return wrapper->sps;
}

static void h264_push(void* ctx1, void* ctx2, OSStatus status,
                      VTEncodeInfoFlags flags, CMSampleBufferRef h264)
{
    apple_wrapper_t* wrapper = (apple_wrapper_t *) ctx1;
    if (h264 == NULL) {
        return;
    }

    uint32_t frametype = video_type_unkown;
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(h264);
    struct my_buffer* mbuf = block_attach(block);
    if (mbuf == NULL) {
        frametype = block_check_nalu_type(block);
        if (video_type_unkown == frametype) {
            if (wrapper->reflost == 0) {
                wrapper->reflost = depends_by_others(h264);
            }
        } else if (reference_able(frametype)) {
            my_assert(depends_by_others(h264));
            wrapper->reflost = 1;
        }
        return;
    }


    CMTime pts = CMSampleBufferGetPresentationTimeStamp(h264);
    frametype = mbuf_check_nalu_type(mbuf);
    if (video_type_unkown == frametype) {
        mbuf->mop->free(mbuf);
        return;
    }

    struct my_buffer* sps_buf = NULL;
    if (frametype == video_type_idr) {
        wrapper->reflost = 0;
        if (wrapper->sps != NULL) {
            sps_buf = init_sps(h264, wrapper);
            if (sps_buf == NULL) {
                wrapper->reflost = 1;
            } else {
                wrapper->sps = NULL;
            }
        }
    }

    if (video_type_sei == frametype || wrapper->reflost == 1) {
        mbuf->mop->free(mbuf);
        return;
    }

    media_buffer* media = (media_buffer *) mbuf->ptr[0];
    media->frametype = frametype;
    media->angle = (uint32_t) ctx2;
    media->pts = pts.value;
    media->fragment[0] = 0;
    media->fragment[1] = 1;
    media->iden = wrapper->iden;
    media->seq = (++wrapper->seq);
    media->pptr_cc = wrapper->out;

    lock(&wrapper->lck);
    if (sps_buf != NULL) {
        media_buffer* media2 = (media_buffer *) sps_buf->ptr[0];
        media2->pts = media->pts;
        media2->angle = media->angle;
        list_add_tail(&sps_buf->head, &wrapper->head);
        wrapper->frames += 1;
        stream_begin(sps_buf);
    }
    list_add_tail(&mbuf->head, &wrapper->head);
    wrapper->frames += 1;
    unlock(&wrapper->lck);
}

static VTCompressionSessionRef create_session(video_format* fmt, apple_wrapper_t* wrapper)
{
    int32_t v = kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
    CFNumberRef pixelref = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);

    v = fmt->pixel->size->width;
    CFNumberRef wref = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);

    v = fmt->pixel->size->height;
    CFNumberRef href = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);

    const void* keys[] = {
        kCVPixelBufferPixelFormatTypeKey,
        kCVPixelBufferWidthKey,
        kCVPixelBufferHeightKey
    };
    const void* values[] = {pixelref, wref, href};
    CFDictionaryRef dict = CFDictionaryCreate(NULL, keys, values, 3, NULL, NULL);

    VTCompressionSessionRef session = NULL;
    OSStatus status = VTCompressionSessionCreate(NULL, fmt->pixel->size->width, fmt->pixel->size->height,
                                                 kCMVideoCodecType_H264, NULL, dict, NULL,
                                                 h264_push, wrapper, &session);
    if (dict != NULL) {
        CFRelease(dict);
    }
    CFRelease(wref);
    CFRelease(href);
    CFRelease(pixelref);
    check_status(status, return NULL);

    status = VTSessionSetProperty(session, kVTCompressionPropertyKey_H264EntropyMode, kVTH264EntropyMode_CABAC);
    check_status(status, (void) 0);
    status = VTSessionSetProperty(session, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_Main_AutoLevel);
    check_status(status, (void) 0);
    status = VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    check_status(status, (void) 0);

    v = (int32_t) fmt->caparam->fps;
    CFNumberRef ref = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
    status = VTSessionSetProperty(session, kVTCompressionPropertyKey_ExpectedFrameRate, ref);
    check_status(status, (void) 0);
    CFRelease(ref);

    v = (int32_t) fmt->caparam->gop;
    ref = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
    status = VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxKeyFrameInterval, ref);
    check_status(status, (void) 0);
    CFRelease(ref);

    v = (int32_t) fmt->caparam->kbps * 1000;
    ref = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
    status = VTSessionSetProperty(session, kVTCompressionPropertyKey_AverageBitRate, ref);
    check_status(status, (void) 0);
    CFRelease(ref);

    v = 1;
    ref = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
    status = VTSessionSetProperty(session, kVTCompressionPropertyKey_FieldCount, ref);
    check_status(status, (void) 0);
    CFRelease(ref);

    return session;
}

static void session_free(apple_wrapper_t* wrapper)
{
    if (wrapper->session != NULL) {
        VTCompressionSessionCompleteFrames(wrapper->session, kCMTimeIndefinite);
        VTCompressionSessionInvalidate(wrapper->session);
        CFRelease(wrapper->session);
        wrapper->session = NULL;
    }
}

static void apple_free(void* addr)
{
    apple_wrapper_t* wrapper = (apple_wrapper_t *) addr;
    session_free(wrapper);
    free_buffer(&wrapper->head);
    if (wrapper->sps != NULL) {
        wrapper->sps->mop->free(wrapper->sps);
    }
    heap_free(wrapper);
}

static void apple_close(void* any)
{
    apple_wrapper_t* wrapper = (apple_wrapper_t *) any;
    my_handle* handle = wrapper->self;
    wrapper->self = NULL;
    handle_dettach(handle);
}

static void* apple_open(fourcc** in, fourcc** out)
{
    errno = ENOMEM;
    apple_wrapper_t* wrapper = heap_alloc(wrapper);
    if (wrapper == NULL) {
        return NULL;
    }

    wrapper->sps = mbuf_alloc_2(sizeof(media_buffer) + 512);
    if (wrapper->sps == NULL) {
        heap_free(wrapper);
    }
    wrapper->sps->ptr[1] += sizeof(media_buffer);
    wrapper->sps->length -= sizeof(media_buffer);

    INIT_LIST_HEAD(&wrapper->head);
    wrapper->lck = lock_val;
    wrapper->reflost = 1;
    wrapper->iden = media_id_unkown;
    wrapper->seq = 0;
    wrapper->frames = 0;
    wrapper->in = in;
    wrapper->out = out;
    wrapper->self = handle_attach(wrapper, apple_free);
    if (wrapper->self == NULL) {
        wrapper->sps->mop->free(wrapper->sps);
        heap_free(wrapper);
        return NULL;
    }

    errno = EFAILED;
    wrapper->session = create_session(to_video_format(in), wrapper);
    if (wrapper->session == NULL) {
        apple_close(wrapper);
        wrapper = NULL;
    }
    return wrapper;
}

static int32_t apple_write(void* handle, struct my_buffer* mbuf, struct list_head* head, int32_t* delay)
{
    (void) delay;
    apple_wrapper_t* wrapper = (apple_wrapper_t *) handle;
    if (mbuf == NULL) {
        if (wrapper->session != NULL) {
            VTCompressionSessionCompleteFrames(wrapper->session, kCMTimeIndefinite);
        }
    } else {
        my_assert(mbuf->length == 0);
        media_buffer* media = (media_buffer *) mbuf->ptr[0];
        wrapper->iden = media->iden;
        uint32_t angle = media->angle;
        CMTime pts = CMTimeMake((int64_t) media->pts, 1000);

        video_buffer_t* buffer = (video_buffer_t *) (mbuf->ptr[0] + sizeof(media_buffer));
        CVPixelBufferRef pixelbuf = CVPixelBufferRetain(buffer->pixelbuf);
        mbuf->mop->free(mbuf);

        if (__builtin_expect(wrapper->session == NULL, 0)) {
            wrapper->session = create_session(to_video_format(wrapper->in), wrapper);
        } else {
            CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(wrapper->session);
            if (__builtin_expect(pool == NULL, 0)) {
                session_free(wrapper);
            }
        }

        if (__builtin_expect(wrapper->session != NULL, 1)) {
            OSStatus status = VTCompressionSessionEncodeFrame(wrapper->session, pixelbuf,
                                        pts, kCMTimeInvalid, NULL, (void *) (intptr_t) angle, NULL);
            check_status(status, (void) 0);
        }
        CFRelease(pixelbuf);
    }

    int32_t n = 0;
    lock(&wrapper->lck);
    if (wrapper->frames > 0) {
        list_join(head, &wrapper->head);
        list_del_init(&wrapper->head);
        n = wrapper->frames;
        wrapper->frames = 0;
    }
    unlock(&wrapper->lck);
    return n;
}

static mpu_operation apple_ops = {
    apple_open,
    apple_write,
    apple_close
};

media_process_unit apple_h264_enc = {
    &raw_0x0.cc,
    &h264_0x0.cc,
    &apple_ops,
    1,
    mpu_encoder,
    "apple_h264_enc"
};

void probe_apple_h264_enc()
{
    double ver = ios_version();
    if (ver < 8.0) {
        apple_h264_enc.ops = NULL;
    } else {
        apple_h264_enc.ops = &apple_ops;
    }
}

#endif
