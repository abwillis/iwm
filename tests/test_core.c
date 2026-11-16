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

int main(void)
{
    const struct iwm_device_config *cfg;
    struct iwm_adapter a;
    struct mock_hw hw;
    struct iwm_os_ops ops;
    struct iwm_fw_session fw;
    struct iwm_fw_begin begin;
    struct iwm_fw_chunk *chunk;
    struct iwm_oid_request oid;
    uint32_t value;

    cfg = iwm_lookup_device(0x8086, 0x9df0);
    assert(cfg && cfg->family == IWM_FAMILY_9000 && cfg->integrated);
    assert(cfg->xtal_latency == 650 && cfg->phy_flags == 0);
    assert(iwm_lookup_device(0x8086, 0xa370) == cfg + 1);
    cfg = iwm_lookup_device(0x8086, 0x30dc);
    assert(cfg && cfg->xtal_latency == 2820 && cfg->integrated);
    cfg = iwm_lookup_device(0x8086, 0x31dc);
    assert(cfg && cfg->xtal_latency == 670 &&
           (cfg->phy_flags & IWM_CFG_SHARED_CLK));
    assert(iwm_lookup_device(0x8087, 0x9df0) == NULL);

    memset(&hw, 0, sizeof(hw));
    memset(&ops, 0, sizeof(ops));
    ops.mmio_read32 = mock_read;
    ops.mmio_write32 = mock_write;
    ops.delay_us = mock_delay;
    hw.regs[0x28 >> 2] = 0x1234;
    iwm_adapter_init(&a, &ops, &hw);
    assert(iwm_adapter_probe(&a, 0x8086, 0x9df0, 0xF0000000UL, 11) == IWM_OK);
    assert(a.ring_plan.rx_count == IWM_RX_MQ_RING_COUNT);

    /* Validation-only session: CRC and ordering, nothing retained. */
    iwm_fw_session_init(&fw, NULL);
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
    /* Without a store there is nothing to load. */
    assert(iwm_fw_start_load(&fw) == IWM_ENOTSUP);
    assert(fw.state == IWM_FW_READY);
    free(chunk);

    /* Factory MAC from the CSR window (base 0x380). Values give the
     * address Linux reported for this card: d4:3b:04:c3:04:74. */
    {
        uint32_t raw[4];
        /* STRAP empty: fall back to OTP. */
        hw.regs[0x388 >> 2 & 63] = 0;
        hw.regs[0x38c >> 2 & 63] = 0;
        hw.regs[0x380 >> 2 & 63] = 0xd43b04c3UL;
        hw.regs[0x384 >> 2 & 63] = 0x00000474UL;
        assert(iwm_adapter_read_csr_mac(&a, 0x380, raw) == IWM_OK);
        assert(raw[2] == 0xd43b04c3UL && raw[3] == 0x474UL);
        assert(a.mac_valid && a.mac[0] == 0xd4 && a.mac[1] == 0x3b &&
               a.mac[2] == 0x04 && a.mac[3] == 0xc3 &&
               a.mac[4] == 0x04 && a.mac[5] == 0x74);
        /* A valid STRAP address wins over OTP. */
        hw.regs[0x388 >> 2 & 63] = 0x02112233UL;
        hw.regs[0x38c >> 2 & 63] = 0x00004455UL;
        assert(iwm_adapter_read_csr_mac(&a, 0x380, NULL) == IWM_OK);
        assert(a.mac[0] == 0x02 && a.mac[5] == 0x55);
        /* Multicast or all-zero in both places is refused. */
        a.mac_valid = 0;
        hw.regs[0x388 >> 2 & 63] = 0x01000000UL;
        hw.regs[0x380 >> 2 & 63] = 0;
        hw.regs[0x384 >> 2 & 63] = 0;
        assert(iwm_adapter_read_csr_mac(&a, 0x380, NULL) == IWM_ENODEV);
        assert(!a.mac_valid);
        assert(iwm_adapter_read_csr_mac(&a, 0, NULL) == IWM_EINVAL);
    }

    memset(&oid, 0, sizeof(oid));
    oid.oid = OID_PRIVATE_WRAPPER_ISWIRELESS;
    oid.data = &value;
    oid.data_length = sizeof(value);
    assert(iwm_genmac_oid_get(&a, &oid) == IWM_OK && value == 1);
    puts("core tests passed");
    return 0;
}


