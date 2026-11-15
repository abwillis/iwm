/* SPDX-License-Identifier: GPL-2.0-only */
/* Firmware-protocol regression tests through the OS/2 IOCTL path, now with a
 * bound firmware store and a TLV-valid synthetic image. */
#include "assert.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "iwm_os2.h"
#include "iwm_test_blob.h"

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
    struct mem_store ms;
    struct iwm_blob store;
    uint8_t image[160];
    uint32_t len, crc;

    memset(&ops, 0, sizeof(ops));
    ops.mmio_read32 = rd;
    ops.mmio_write32 = wr;
    ops.delay_us = dl;
    regs[0x28 >> 2] = 0x1234;
    iwm_adapter_init(&a, &ops, NULL);
    assert(iwm_adapter_probe(&a, 0x8086, 0x9df0, 0xF0000000UL, 11) == IWM_OK);
    memset(&ms, 0, sizeof(ms));
    iwm_blob_bind(&store, &mem_store_ops, &ms);
    assert(iwm_adapter_set_fw_store(&a, &store) == IWM_OK);

    len = build_image(image, 1);
    crc = iwm_crc32_update(0xFFFFFFFFUL, image, len) ^ 0xFFFFFFFFUL;
    chunk = (struct iwm_fw_chunk *)calloc(1, sizeof(*chunk));
    assert(chunk);
    chunk->length = (uint16_t)len;
    memcpy(chunk->data, image, len);
    memset(&begin, 0, sizeof(begin));
    begin.magic = IWM_FW_MAGIC;
    begin.total_length = len;
    memset(&io, 0, sizeof(io));
    io.category = IWM_IOCTL_CATEGORY;

    /* Expected CRC 0 must not bypass verification. */
    begin.expected_crc32 = 0;
    io.function = IWM_IOCTL_FW_BEGIN; io.data = &begin; io.data_length = sizeof(begin);
    assert(iwm_os2_ioctl_dispatch(&a, &io) == IWM_OK);
    io.function = IWM_IOCTL_FW_DATA; io.data = chunk;
    io.data_length = (uint32_t)IWM_FW_CHUNK_HEADER + len;
    assert(iwm_os2_ioctl_dispatch(&a, &io) == IWM_OK);
    io.function = IWM_IOCTL_FW_COMMIT; io.data = NULL; io.data_length = 0;
    assert(iwm_os2_ioctl_dispatch(&a, &io) == IWM_ECRC);
    assert(a.firmware.state == IWM_FW_FAILED);

    /* DATA shorter than header + chunk->length is rejected, including a
     * buffer shorter than the header itself. */
    begin.expected_crc32 = crc;
    io.function = IWM_IOCTL_FW_BEGIN; io.data = &begin; io.data_length = sizeof(begin);
    assert(iwm_os2_ioctl_dispatch(&a, &io) == IWM_OK);
    io.function = IWM_IOCTL_FW_DATA; io.data = chunk;
    io.data_length = (uint32_t)IWM_FW_CHUNK_HEADER + len - 1U;
    assert(iwm_os2_ioctl_dispatch(&a, &io) == IWM_EINVAL);
    io.data_length = (uint32_t)IWM_FW_CHUNK_HEADER - 1U;
    assert(iwm_os2_ioctl_dispatch(&a, &io) == IWM_EINVAL);
    assert(a.firmware.accepted == 0);
    io.data_length = (uint32_t)IWM_FW_CHUNK_HEADER + len;
    assert(iwm_os2_ioctl_dispatch(&a, &io) == IWM_OK);
    assert(memcmp(ms.buf, image, len) == 0);

    /* COMMIT verifies, catalogs, -> LOADING; scan refused until ALIVE. */
    io.function = IWM_IOCTL_FW_COMMIT; io.data = NULL; io.data_length = 0;
    assert(iwm_os2_ioctl_dispatch(&a, &io) == IWM_OK);
    assert(a.firmware.state == IWM_FW_LOADING);
    assert(a.firmware.catalog.runtime_count == 1);
    assert(a.firmware.catalog.init_count == 1);
    memset(&oid, 0, sizeof(oid));
    oid.oid = OID_802_11_BSSID_LIST_SCAN;
    assert(iwm_genmac_oid_set(&a, &oid) == IWM_ESTATE);
    assert(iwm_adapter_fw_alive(&a) == IWM_OK);
    assert(a.firmware.state == IWM_FW_RUNNING);
    assert(iwm_genmac_oid_set(&a, &oid) == IWM_OK);

    /* RUNNING image cannot be overwritten, aborted or have its store swapped. */
    io.function = IWM_IOCTL_FW_BEGIN; io.data = &begin; io.data_length = sizeof(begin);
    assert(iwm_os2_ioctl_dispatch(&a, &io) == IWM_EBUSY);
    io.function = IWM_IOCTL_FW_ABORT; io.data = NULL; io.data_length = 0;
    assert(iwm_os2_ioctl_dispatch(&a, &io) == IWM_EBUSY);
    assert(iwm_adapter_set_fw_store(&a, &store) == IWM_EBUSY);

    /* Stopping the device releases the session but keeps the store bound. */
    assert(iwm_stop_device(&a) == IWM_OK);
    assert(a.firmware.state == IWM_FW_EMPTY && a.firmware.store == &store);
    io.function = IWM_IOCTL_FW_BEGIN; io.data = &begin; io.data_length = sizeof(begin);
    assert(iwm_os2_ioctl_dispatch(&a, &io) == IWM_OK);
    /* Caller abort during RECEIVING discards staged bytes. */
    io.function = IWM_IOCTL_FW_ABORT; io.data = NULL; io.data_length = 0;
    assert(iwm_os2_ioctl_dispatch(&a, &io) == IWM_OK);
    assert(store.length == 0 && ms.buf == NULL);

    /* An image without a runtime section is rejected at COMMIT. */
    len = build_image(image, 0);
    crc = iwm_crc32_update(0xFFFFFFFFUL, image, len) ^ 0xFFFFFFFFUL;
    begin.total_length = len;
    begin.expected_crc32 = crc;
    chunk->length = (uint16_t)len;
    memcpy(chunk->data, image, len);
    io.function = IWM_IOCTL_FW_BEGIN; io.data = &begin; io.data_length = sizeof(begin);
    assert(iwm_os2_ioctl_dispatch(&a, &io) == IWM_OK);
    io.function = IWM_IOCTL_FW_DATA; io.data = chunk;
    io.data_length = (uint32_t)IWM_FW_CHUNK_HEADER + len;
    assert(iwm_os2_ioctl_dispatch(&a, &io) == IWM_OK);
    io.function = IWM_IOCTL_FW_COMMIT; io.data = NULL; io.data_length = 0;
    assert(iwm_os2_ioctl_dispatch(&a, &io) == IWM_EFORMAT);
    assert(a.firmware.state == IWM_FW_FAILED);
    assert(a.firmware.catalog.diag.reason == IWM_FWP_NO_RUNTIME);

    assert(iwm_adapter_set_fw_store(&a, NULL) == IWM_OK);
    iwm_blob_reset(&store);
    free(chunk);
    puts("fw-state fix tests passed");
    return 0;
}
