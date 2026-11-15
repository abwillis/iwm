/* SPDX-License-Identifier: GPL-2.0-only */
#include "iwm_fw_proto.h"

uint32_t iwm_crc32_update(uint32_t crc, const void *data, size_t length)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t i;
    unsigned bit;
    for (i = 0; i < length; ++i) {
        crc ^= p[i];
        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1U)));
    }
    return crc;
}

