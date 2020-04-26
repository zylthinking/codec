
#include "mp4.h"

static void over_write_annexb(annexb_to_mp4_t* wrapper, uint32_t nb)
{
    nb = htonl(nb - 4);
    if (wrapper->nbptr2 == NULL) {
        my_assert(wrapper->zero1 == 4);
        *wrapper->nbptr1 = nb;
    } else {
        char* pch1 = (char *) wrapper->nbptr1;
        char* pch2 = (char *) &nb;
        switch (wrapper->zero1) {
            case 3: *pch1++ = *pch2++;
            case 2: *pch1++ = *pch2++;
            case 1: *pch1++ = *pch2++; break;
            default: my_assert(0);
        }

        pch1 = (char *) wrapper->nbptr2;
        switch (4 - wrapper->zero1) {
            case 1: *pch1++ = *pch2++;
            case 2: *pch1++ = *pch2++;
            case 3: *pch1++ = *pch2++;
        }
    }
}

static void check_broken_annexb(annexb_to_mp4_t* wrapper, char* payload, intptr_t bytes)
{
    payload += bytes;
    while (bytes > 0 && wrapper->zero2 < 3 && payload[-1] == 0) {
        ++wrapper->zero2;
        --bytes;
        wrapper->nbptr3 = (uint32_t *) (--payload);
    }
}

void annexb_to_mp4(annexb_to_mp4_t* wrapper, char* payload, intptr_t bytes)
{
    static char annexb[] = {0, 0, 0, 1};
    int n = 1, nb;
    my_assert(wrapper->nbptr1 != NULL);

    while (wrapper->zero2 > 0) {
        my_assert(wrapper->nbptr3 != NULL);

        nb = 4 - wrapper->zero2;
        if (bytes <= nb) {
            // assume only the last fragment have size of less than 3
            // if bytes == 3, then we have a full annexb but no payload
            // it is impossible
            my_assert(bytes != nb);
            wrapper->len += bytes;
            return;
        }

        n = memcmp(payload, &annexb[wrapper->zero2], nb);
        if (n == 0) {
            my_assert(wrapper->len >= 4 + wrapper->zero2);
            over_write_annexb(wrapper, wrapper->len - wrapper->zero2);

            if (wrapper->nbptr3 == NULL) {
                wrapper->nbptr1 = (uint32_t *) payload;
                wrapper->nbptr2 = NULL;
                wrapper->zero1 = 4;
            } else {
                wrapper->nbptr1 = wrapper->nbptr3;
                wrapper->nbptr2 = (uint32_t *) payload;
                wrapper->zero1 = wrapper->zero2;
            }
            wrapper->len = 4;
            payload += nb;
            bytes -= nb;
            wrapper->zero2 = 0;
            wrapper->nbptr3 = NULL;
            break;
        }

        if (--wrapper->zero2) {
            wrapper->nbptr3 = (uint32_t *) (((intptr_t) wrapper->nbptr3) + 1);
        } else {
            wrapper->nbptr3 = NULL;
        }
    }

    while (bytes > 0) {
        char* pch = (char *) memmem(payload, bytes, annexb, 4);
        if (pch == NULL) {
            wrapper->len += bytes;
            check_broken_annexb(wrapper, payload, bytes);
            break;
        }

        nb = (int32_t) (pch - payload);
        over_write_annexb(wrapper, wrapper->len + nb);

        wrapper->nbptr1 = (uint32_t *) pch;
        wrapper->nbptr2 = NULL;
        wrapper->zero1 = 4;
        wrapper->len = 4;

        payload += (nb + 4);
        bytes -= (nb + 4);
    }
}

void annexb_to_mp4_begin(annexb_to_mp4_t* wrapper, char* payload, intptr_t bytes)
{
    wrapper->nbptr1 = (uint32_t *) payload;
    wrapper->len = 4;
    wrapper->zero1 = 4;

    wrapper->nbptr2 = NULL;
    wrapper->nbptr3 = NULL;
    wrapper->zero2 = 0;
    annexb_to_mp4(wrapper, payload + 4, bytes - 4);
}

void annexb_to_mp4_end(annexb_to_mp4_t* wrapper)
{
    over_write_annexb(wrapper, wrapper->len);
}
