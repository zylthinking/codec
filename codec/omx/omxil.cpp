
#if defined(__ANDROID__)
#include "omxil.h"

void qcom_low_lantency(IOMX* omx, IOMX::node_id id)
{
#if 0
    QOMX_VIDEO_DECODER_PICTURE_ORDER params;
    InitOMXParams(&params);
    params.nPortIndex = kPortIndexOutput;
    params.eOutputPictureOrder = QOMX_VIDEO_DECODE_ORDER;
    status_t err = omx->setParameter(id, OMX_QcomIndexParamVideoDecoderPictureOrder, &params, sizeof(params));
    if (err != OK) {
        logmsg("openmax qcom QOMX_VIDEO_DECODE_ORDER failed err = %x", (int) err);
    }
#endif
}

#endif