/* SPDX-License-Identifier: GPL-2.0-only */
/* Regression tests for the four firmware-state fixes. */
#include "assert.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "iwm_os2.h"

static uint32_t regs[64];
static uint32_t rd(void *c, uint32_t o) { (void)c; return regs[(o >> 2) & 63]; }
static void wr(void *c, uint32_t o, uint32_t v) { (void)c; regs[(o >> 2) & 63] = v; }
static void dl(void *c, uint32_t u) { (void)c; (void)u; }

int main(void)
{
    struct iwm_adapter a;
    struct iwm_os_ops ops;
    struct iwm_fw_begin begin;
    struct iwm_fw_chunk *chunk;
    struct iwm_os2_ioctl io;
    struct iwm_oid_request oid;

    memset(&ops, 0, sizeof(ops));
    ops.mmio_read32 = rd;
    ops.mmio_write32 = wr;
    ops.delay_us = dl;
    regs[0x28 >> 2] = 0x1234;
    iwm_adapter_init(&a, &ops, NULL);
    assert(iwm_adapter_probe(&a, 0x8086, 0x9df0, 0xF0000000UL, 11) == IWM_OK);

    chunk = (struct iwm_fw_chunk *)calloc(1, sizeof(*chunk));
    assert(chunk);
    chunk->length = 4;
    chunk->data[0] = 1; chunk->data[1] = 2;
    chunk->data[2] = 3; chunk->data[3] = 4;
    memset(&begin, 0, sizeof(begin));
    begin.magic = IWM_FW_MAGIC;
    begin.total_length = 4;
    memset(&io, 0, sizeof(io));
    io.category = IWM_IOCTL_CATEGORY;

    /* Issue 3: expected CRC 0 must not bypass verification. */
    begin.expected_crc32 = 0;
    io.function = IWM_IOCTL_FW_BEGIN; io.data = &begin; io.data_length = sizeof(begin);
    assert(iwm_os2_ioctl_dispatch(&a, &io) == IWM_OK);
    io.function = IWM_IOCTL_FW_DATA; io.data = chunk;
    io.data_length = (uint32_t)IWM_FW_CHUNK_HEADER + 4U;
    assert(iwm_os2_ioctl_dispatch(&a, &io) == IWM_OK);
    io.function = IWM_IOCTL_FW_COMMIT; io.data = NULL; io.data_length = 0;
    assert(iwm_os2_ioctl_dispatch(&a, &io) == IWM_ECRC);
    assert(a.firmware.state == IWM_FW_FAILED);

    /* Issue 2: DATA shorter than header + chunk->length is rejected. */
    begin.expected_crc32 = 0xB63CFBCDUL; /* CRC32 of 01 02 03 04 */
    io.function = IWM_IOCTL_FW_BEGIN; io.data = &begin; io.data_length = sizeof(begin);
    assert(iwm_os2_ioctl_dispatch(&a, &io) == IWM_OK);
    io.function = IWM_IOCTL_FW_DATA; io.data = chunk;
    io.data_length = (uint32_t)IWM_FW_CHUNK_HEADER + 3U;
    assert(iwm_os2_ioctl_dispatch(&a, &io) == IWM_EINVAL);
    assert(a.firmware.accepted == 0);
    io.data_length = (uint32_t)IWM_FW_CHUNK_HEADER + 4U;
    assert(iwm_os2_ioctl_dispatch(&a, &io) == IWM_OK);

    /* Issue 1: COMMIT -> LOADING; scan refused until ALIVE -> RUNNING. */
    io.function = IWM_IOCTL_FW_COMMIT; io.data = NULL; io.data_length = 0;
    assert(iwm_os2_ioctl_dispatch(&a, &io) == IWM_OK);
    assert(a.firmware.state == IWM_FW_LOADING);
    memset(&oid, 0, sizeof(oid));
    oid.oid = OID_802_11_BSSID_LIST_SCAN;
    assert(iwm_genmac_oid_set(&a, &oid) == IWM_ESTATE);
    assert(iwm_adapter_fw_alive(&a) == IWM_OK);
    assert(a.firmware.state == IWM_FW_RUNNING);
    assert(iwm_genmac_oid_set(&a, &oid) == IWM_OK);

    /* Issue 4: RUNNING image cannot be overwritten or aborted from ring 3. */
    io.function = IWM_IOCTL_FW_BEGIN; io.data = &begin; io.data_length = sizeof(begin);
    assert(iwm_os2_ioctl_dispatch(&a, &io) == IWM_EBUSY);
    io.function = IWM_IOCTL_FW_ABORT; io.data = NULL; io.data_length = 0;
    assert(iwm_os2_ioctl_dispatch(&a, &io) == IWM_EBUSY);

    /* Stopping the device releases the session for a reload. */
    assert(iwm_stop_device(&a) == IWM_OK);
    assert(a.firmware.state == IWM_FW_EMPTY);
    io.function = IWM_IOCTL_FW_BEGIN; io.data = &begin; io.data_length = sizeof(begin);
    assert(iwm_os2_ioctl_dispatch(&a, &io) == IWM_OK);

    free(chunk);
    puts("fw-state fix tests passed");
    return 0;
}
