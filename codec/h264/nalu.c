
#include "nalu.h"
#include "bits.h"
#include "fmt.h"
#include <string.h>

static void skip_one_golomb(const uint8_t* const base, uint32_t* offset)
{
    uint32_t zeros = 0;
    while (0 == get_bit_at(base, (*offset)++)) zeros++;
    (*offset) += zeros;
}

static uint32_t decode_ue_golomb(const uint8_t* const base, uint32_t* offset)
{
    uint32_t zeros = 0;
    while (0 == get_bit_at(base, (*offset)++)) zeros++;

    uint32_t info = 1 << zeros;
    for (int32_t i = zeros - 1; i >= 0; i--) {
        info |= get_bit_at(base, (*offset)++) << i;
    }
    return (info - 1);
}

static const uint8_t* nalu_bit_stream(uint8_t* pch, const uint8_t* nalu, int32_t bytes)
{
    int score = 0;
    for (int i = 0; i < bytes; ++i) {
        if (nalu[i] == 0) {
            score++;
            my_assert(score < 3);
            *(pch++) = nalu[i];
            continue;
        }

        if (nalu[i] != 3 || score != 2) {
            *(pch++) = nalu[i];
        }
        score = 0;
    }
    return pch;
}

//seq_parameter_set_rbsp
int32_t num_ref_frames(const uint8_t* nalu, int32_t bytes)
{
    uint8_t buffer[64] = {0, 0, 1};
    my_assert(nalu[0] == 0 && nalu[1] == 0);
    if (nalu[2] == 0) {
        my_assert(nalu[3] == 1);
        nalu += 4;
        bytes -= 4;
    } else {
        nalu += 3;
        bytes -= 3;
    }

    int nalu_type = (nalu[0] & 0x1f);
    if (nalu_type != 7) {
        return -1;
    }

    const uint8_t* next = (const uint8_t *) memmem(nalu, bytes, buffer, 3);
    if (next != NULL) {
        if (next[-1] == 0) {
            next -= 1;
        }
        bytes = (int32_t) (next - nalu);
    }

    my_assert2(bytes < 64, "sps > 64 bytes");
    nalu += 1;
    bytes -= 1;

    nalu_bit_stream(buffer, nalu, bytes);
    nalu = &buffer[3];

    uint32_t offset = 0;
    skip_one_golomb(nalu, &offset);
    skip_one_golomb(nalu, &offset);

    uint32_t pic_order_cnt_type = decode_ue_golomb(nalu, &offset);
    if (pic_order_cnt_type == 0) {
        skip_one_golomb(nalu, &offset);
    } else if (pic_order_cnt_type == 1) {
        offset += 1;
        skip_one_golomb(nalu, &offset);
        skip_one_golomb(nalu, &offset);

        uint32_t nr = decode_ue_golomb(nalu, &offset);
        for (int i = 0; i < nr; ++i) {
            skip_one_golomb(nalu, &offset);
        }
    }

    uint32_t nr = decode_ue_golomb(nalu, &offset);
    return nr;
}

int32_t none_dir_frametype(char* nalu, intptr_t bytes)
{
    my_assert(nalu[0] == 0 && nalu[1] == 0);
    if (nalu[2] == 0) {
        my_assert(nalu[3] == 1);
        nalu += 4;
        bytes -= 4;
    } else {
        nalu += 3;
        bytes -= 3;
    }

    int32_t ref = nalu[0] & 0x60;
    uint32_t offset = 0;
    const uint8_t* intp = (const uint8_t *) (nalu + 1);
    skip_one_golomb(intp, &offset);
    uint32_t type = decode_ue_golomb(intp, &offset);
    int32_t frametype;
    if (type == 0 || type == 3 || type == 5 || type == 8) {
        frametype = video_type_p;
    } else if (type == 2 || type == 4 || type == 7 || type == 9) {
        frametype = video_type_i;
    } else if (ref == 0) {
        frametype = video_type_b;
    } else {
        frametype = video_type_bref;
    }
    return frametype;
}

