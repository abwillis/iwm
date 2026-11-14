/* SPDX-License-Identifier: GPL-2.0-only */
/* Host-only memory firmware store and synthetic image builder for tests. */
#ifndef IWM_TEST_BLOB_H
#define IWM_TEST_BLOB_H

#include "stdlib.h"
#include "string.h"
#include "iwm_fw_proto.h"

struct mem_store {
    uint8_t *buf;
    uint32_t len;
    unsigned resets;
};

static int ms_resize(void *c, uint32_t n)
{
    struct mem_store *m = (struct mem_store *)c;
    uint8_t *p = (uint8_t *)realloc(m->buf, n ? n : 1);
    if (!p)
        return IWM_ENOMEM;
    m->buf = p;
    m->len = n;
    memset(m->buf, 0, n ? n : 1);
    return IWM_OK;
}

static int ms_write(void *c, uint32_t off, const uint8_t *d, uint16_t n)
{
    struct mem_store *m = (struct mem_store *)c;
    if (off > m->len || (uint32_t)n > m->len - off)
        return IWM_EOVERFLOW;
    memcpy(m->buf + off, d, n);
    return IWM_OK;
}

static int ms_read(void *c, uint32_t off, uint8_t *d, uint16_t n)
{
    struct mem_store *m = (struct mem_store *)c;
    if (off > m->len || (uint32_t)n > m->len - off)
        return IWM_EOVERFLOW;
    memcpy(d, m->buf + off, n);
    return IWM_OK;
}

static void ms_reset(void *c)
{
    struct mem_store *m = (struct mem_store *)c;
    free(m->buf);
    m->buf = NULL;
    m->len = 0;
    ++m->resets;
}

static const struct iwm_blob_ops mem_store_ops = {
    ms_resize, ms_write, ms_read, ms_reset
};

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Header + NUM_OF_CPU(2) + secure INIT + secure RT (+ separator).
 * Returns the image length; buf must hold at least 160 bytes. */
static uint32_t build_image(uint8_t *buf, int with_runtime)
{
    uint32_t pos = IWM_UCODE_HEADER_SIZE;
    memset(buf, 0, 160);
    put_le32(buf + 4, IWM_UCODE_TLV_MAGIC);
    put_le32(buf + IWM_UCODE_VER_OFFSET, 46);
    put_le32(buf + IWM_UCODE_BUILD_OFFSET, 0x11223344UL);
    put_le32(buf + pos, IWM_UCODE_TLV_NUM_OF_CPU);
    put_le32(buf + pos + 4, 4);
    put_le32(buf + pos + 8, 2);
    pos += 12;
    put_le32(buf + pos, IWM_UCODE_TLV_SECURE_SEC_INIT);
    put_le32(buf + pos + 4, 8);
    put_le32(buf + pos + 8, 0x00400000UL);
    put_le32(buf + pos + 12, 0xAABBCCDDUL);
    pos += 16;
    put_le32(buf + pos, IWM_UCODE_TLV_SECURE_SEC_INIT);
    put_le32(buf + pos + 4, 4);
    put_le32(buf + pos + 8, IWM_UCODE_SECTION_CPU1_CPU2);
    pos += 12;
    if (with_runtime) {
        put_le32(buf + pos, IWM_UCODE_TLV_SECURE_SEC_RT);
        put_le32(buf + pos + 4, 8);
        put_le32(buf + pos + 8, 0x00800000UL);
        put_le32(buf + pos + 12, 0x01020304UL);
        pos += 16;
    }
    return pos;
}

#endif
