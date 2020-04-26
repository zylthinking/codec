
#ifndef omxil_h
#define omxil_h

#if defined(__ANDROID__)
#include "mydef.h"
#include "OMX_Core.h"
#include <media/IOMX.h>

using namespace android;

typedef enum QOMX_VIDEO_PICTURE_ORDER {
    QOMX_VIDEO_DISPLAY_ORDER = 0x0,
    QOMX_VIDEO_DECODE_ORDER = 0x1
} QOMX_VIDEO_PICTURE_ORDER;

typedef struct QOMX_VIDEO_DECODER_PICTURE_ORDER {
    OMX_U32 nSize;
    OMX_VERSIONTYPE nVersion;
    OMX_U32 nPortIndex;
    QOMX_VIDEO_PICTURE_ORDER eOutputPictureOrder;
} QOMX_VIDEO_DECODER_PICTURE_ORDER;


enum {
    kPortIndexBoth   = -1,
    kPortIndexInput  = 0,
    kPortIndexOutput = 1
};

template<class T>
static void InitOMXParams(T* params) {
    params->nSize = sizeof(T);
    params->nVersion.s.nVersionMajor = 1;
#if 1
    params->nVersion.s.nVersionMinor = 1;
#else
    params->nVersion.s.nVersionMinor = 0;
#endif
    params->nVersion.s.nRevision = 0;
    params->nVersion.s.nStep = 0;
}

#define OMX_QcomIndexParamVideoDecoderPictureOrder ((OMX_INDEXTYPE) 0x7F000010)
capi void qcom_low_lantency(IOMX* omx, IOMX::node_id id);

#endif
#endif
