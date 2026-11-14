/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "iwm_genmac.h"

struct mock_hw { uint32_t regs[64]; };
static uint32_t mock_read(void *p, uint32_t off)
{
    struct mock_hw *m = (struct mock_hw *)p;
    return m->regs[(off >> 2) & 63];
}
static void mock_write(void *p, uint32_t off, uint32_t value)
{
    struct mock_hw *m = (struct mock_hw *)p;
    m->regs[(off >> 2) & 63] = value;
}
static void mock_delay(void *p, uint32_t us) { (void)p; (void)us; }

struct sink { uint8_t bytes[64]; size_t used; };
static int consume(void *p, uint32_t off, const uint8_t *data, uint16_t n)
{
    struct sink *s = (struct sink *)p;
    assert(off == s->used);
    assert(s->used + n <= sizeof(s->bytes));
    memcpy(s->bytes + s->used, data, n);
    s->used += n;
    return IWM_OK;
}

int main(void)
{
    const struct iwm_device_config *cfg;
    struct iwm_adapter a;
    struct mock_hw hw;
    struct iwm_os_ops ops;
    struct iwm_fw_session fw;
    struct iwm_fw_begin begin;
    struct iwm_fw_chunk *chunk;
    struct sink sink;
    struct iwm_oid_request oid;
    uint32_t value;

    cfg = iwm_lookup_device(0x8086, 0x9df0);
    assert(cfg && cfg->family == IWM_FAMILY_9000 && cfg->integrated);
    assert(iwm_lookup_device(0x8086, 0xa370) == cfg + 1 ||
           iwm_lookup_device(0x8086, 0xa370) != NULL);

    memset(&hw, 0, sizeof(hw));
    memset(&ops, 0, sizeof(ops));
    ops.mmio_read32 = mock_read;
    ops.mmio_write32 = mock_write;
    ops.delay_us = mock_delay;
    hw.regs[0x28 >> 2] = 0x1234;
    iwm_adapter_init(&a, &ops, &hw);
    assert(iwm_adapter_probe(&a, 0x8086, 0x9df0, 0xF0000000UL, 11) == IWM_OK);
    assert(a.ring_plan.rx_count == IWM_RX_MQ_RING_COUNT);

    memset(&sink, 0, sizeof(sink));
    iwm_fw_session_init(&fw, consume, &sink);
    memset(&begin, 0, sizeof(begin));
    begin.magic = IWM_FW_MAGIC;
    begin.total_length = 4;
    begin.expected_crc32 = 0xB63CFBCDUL; /* CRC32 of 01 02 03 04 */
    assert(iwm_fw_begin(&fw, &begin) == IWM_OK);
    chunk = (struct iwm_fw_chunk *)calloc(1, sizeof(*chunk));
    assert(chunk);
    chunk->length = 4;
    chunk->data[0] = 1; chunk->data[1] = 2;
    chunk->data[2] = 3; chunk->data[3] = 4;
    assert(iwm_fw_push(&fw, chunk) == IWM_OK);
    assert(iwm_fw_commit(&fw) == IWM_OK);
    free(chunk);

    memset(&oid, 0, sizeof(oid));
    oid.oid = OID_PRIVATE_WRAPPER_ISWIRELESS;
    oid.data = &value;
    oid.data_length = sizeof(value);
    assert(iwm_genmac_oid_get(&a, &oid) == IWM_OK && value == 1);
    puts("core tests passed");
    return 0;
}
