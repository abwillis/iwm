/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "iwm_tx.h"

#define CSR_GP_CNTRL             0x024U
#define CSR_HW_REV               0x028U
#define CSR_GP_CLOCK_READY       0x1U
#define CSR_GP_INIT_DONE         0x4U
#define HBUS_TARG_MEM_WADDR      0x410U
#define HBUS_TARG_MEM_WDAT       0x418U
#define HBUS_TARG_PRPH_WADDR     0x444U
#define HBUS_TARG_PRPH_RADDR     0x448U
#define HBUS_TARG_PRPH_WDAT      0x44cU
#define HBUS_TARG_PRPH_RDAT      0x450U
#define HBUS_TARG_WRPTR          0x460U
#define FH_KW_MEM_ADDR_REG       0x197cU
#define FH_MEM_CBBC_QUEUE0       0x19d0U
#define FH_TCSR_CONFIG(ch)       (0x1d00U + 0x20U * (ch))
#define FH_TX_CHICKEN_BITS_REG   0x1e98U
#define SCD_BASE                 0x00a02c00UL
#define SCD_SRAM_BASE_ADDR       (SCD_BASE + 0x000U)
#define SCD_DRAM_BASE_ADDR       (SCD_BASE + 0x008U)
#define SCD_TXFACT               (SCD_BASE + 0x010U)
#define SCD_QUEUE_RDPTR0         (SCD_BASE + 0x068U)
#define SCD_QUEUE_STATUS0        (SCD_BASE + 0x10cU)
#define SCD_AGGR_SEL             (SCD_BASE + 0x248U)

struct mock_hw {
    uint32_t regs[2048];
    uint32_t next_phys;
    uint32_t prph_waddr;
    uint32_t prph_raddr;
    uint32_t mem_waddr;
    uint32_t scd_sram_base;
    uint32_t scd_dram_base;
    uint32_t scd_txfact;
    uint32_t scd_status0;
    uint32_t scd_rdptr0;
    uint32_t scd_aggr;
    uint32_t mem_writes;
    uint32_t first_mem_addr;
    uint32_t last_mem_addr;
    uint32_t wfpm_ctrl;
    int hpm_read_awake;
};

static uint32_t mock_read(void *p, uint32_t off)
{
    struct mock_hw *m = (struct mock_hw *)p;

    if (off == HBUS_TARG_PRPH_RDAT) {
        switch (m->prph_raddr) {
        case SCD_SRAM_BASE_ADDR: return m->scd_sram_base;
        case SCD_DRAM_BASE_ADDR: return m->scd_dram_base;
        case SCD_TXFACT: return m->scd_txfact;
        case SCD_QUEUE_RDPTR0: return m->scd_rdptr0;
        case SCD_QUEUE_STATUS0: return m->scd_status0;
        case SCD_AGGR_SEL: return m->scd_aggr;
        case 0x00a03030UL: return m->wfpm_ctrl;
        default: return 0;
        }
    }
    return m->regs[(off >> 2) & 2047U];
}

static void mock_write(void *p, uint32_t off, uint32_t value)
{
    struct mock_hw *m = (struct mock_hw *)p;
    unsigned i = (off >> 2) & 2047U;

    if (off == HBUS_TARG_PRPH_WADDR) {
        m->prph_waddr = value & 0x00ffffffUL;
        return;
    }
    if (off == HBUS_TARG_PRPH_RADDR) {
        m->prph_raddr = value & 0x00ffffffUL;
        /* HPM_DEBUG read while INIT_DONE is set: the persistence check ran
         * on an awake device, as after Linux probe / FreeBSD attach. */
        if (m->prph_raddr == 0x00a03440UL &&
            (m->regs[CSR_GP_CNTRL >> 2] & CSR_GP_INIT_DONE))
            m->hpm_read_awake = 1;
        return;
    }
    if (off == HBUS_TARG_PRPH_WDAT) {
        switch (m->prph_waddr) {
        case SCD_DRAM_BASE_ADDR: m->scd_dram_base = value; break;
        case SCD_TXFACT: m->scd_txfact = value; break;
        case SCD_QUEUE_RDPTR0: m->scd_rdptr0 = value; break;
        case SCD_QUEUE_STATUS0: m->scd_status0 = value; break;
        case SCD_AGGR_SEL: m->scd_aggr = value; break;
        case 0x00a03030UL: m->wfpm_ctrl = value; break;
        default: break;
        }
        return;
    }
    if (off == HBUS_TARG_MEM_WADDR) {
        m->mem_waddr = value;
        return;
    }
    if (off == HBUS_TARG_MEM_WDAT) {
        if (m->mem_writes == 0)
            m->first_mem_addr = m->mem_waddr;
        m->last_mem_addr = m->mem_waddr;
        ++m->mem_writes;
        return;
    }
    m->regs[i] = value;
    if (off == CSR_GP_CNTRL && (value & CSR_GP_INIT_DONE))
        m->regs[i] |= CSR_GP_CLOCK_READY;
}

static void mock_delay(void *p, uint32_t usec)
{
    (void)p;
    (void)usec;
}

static int mock_dma_alloc(void *p, uint32_t size, uint32_t alignment,
                          struct iwm_dma_block *b)
{
    struct mock_hw *m = (struct mock_hw *)p;
    size_t raw_addr;
    size_t aligned;
    void *raw;

    raw = malloc(size + alignment - 1U);
    if (!raw)
        return IWM_ENOMEM;
    raw_addr = (size_t)raw;
    aligned = (raw_addr + alignment - 1U) & ~((size_t)alignment - 1U);
    memset(b, 0, sizeof(*b));
    b->alloc_vaddr = raw;
    b->alloc_size = size + alignment - 1U;
    b->vaddr = (void *)aligned;
    m->next_phys = IWM_ALIGN_UP(m->next_phys, alignment);
    b->paddr = m->next_phys;
    b->size = size;
    b->alignment = alignment;
    m->next_phys += IWM_ALIGN_UP(size, alignment);
    memset(b->vaddr, 0, size);
    return IWM_OK;
}

static void mock_dma_free(void *p, struct iwm_dma_block *b)
{
    (void)p;
    free(b->alloc_vaddr ? b->alloc_vaddr : b->vaddr);
    memset(b, 0, sizeof(*b));
}

int main(void)
{
    struct mock_hw hw;
    struct iwm_os_ops ops;
    struct iwm_adapter adapter;
    struct iwm_tx_transport tx;
    struct iwm_tx_debug debug;
    uint32_t saved_tfd_phys;

    memset(&hw, 0, sizeof(hw));
    memset(&ops, 0, sizeof(ops));
    hw.next_phys = 0x10000000UL;
    hw.scd_sram_base = 0x00800000UL;
    hw.scd_aggr = 0xffffffffUL;
    hw.regs[CSR_HW_REV >> 2] = 0x312U;
    ops.mmio_read32 = mock_read;
    ops.mmio_write32 = mock_write;
    ops.delay_us = mock_delay;
    ops.dma_alloc = mock_dma_alloc;
    ops.dma_free = mock_dma_free;

    iwm_adapter_init(&adapter, &ops, &hw);
    assert(iwm_adapter_probe(&adapter, 0x8086, 0xa370, 0, 54) == IWM_OK);
    iwm_tx_init(&tx, &adapter);
    assert(iwm_tx_start_scheduler(&tx) == IWM_ESTATE);
    assert(iwm_tx_alloc(&tx) == IWM_OK);
    assert(tx.scd_bc_table.size == IWM_TX_NUM_QUEUES *
           IWM_TX_BC_ENTRIES * IWM_TX_BC_ENTRY_SIZE);
    assert(tx.cmd_tfds.size == IWM_TX_HW_QUEUE_SIZE * IWM_TX_TFD_SIZE);
    assert(iwm_prepare_card_hw(&adapter) == IWM_OK);
    assert(hw.hpm_read_awake);
    assert((hw.wfpm_ctrl & 0x80000000UL) != 0);
    assert(adapter.wfpm_rc == IWM_OK && adapter.wfpm_ctrl[1] == hw.wfpm_ctrl);

    saved_tfd_phys = tx.cmd_tfds.paddr;
    tx.cmd_tfds.paddr += 4U;
    assert(iwm_tx_prepare(&tx) == IWM_ERANGE);
    tx.cmd_tfds.paddr = saved_tfd_phys;

    assert(iwm_tx_prepare(&tx) == IWM_OK);
    assert(tx.prepared && !tx.scheduler_started);
    assert(hw.regs[FH_KW_MEM_ADDR_REG >> 2] == tx.keep_warm.paddr >> 4);
    assert(hw.regs[FH_MEM_CBBC_QUEUE0 >> 2] == tx.cmd_tfds.paddr >> 8);
    assert(hw.regs[(FH_MEM_CBBC_QUEUE0 + 4U) >> 2] == tx.aux_tfds.paddr >> 8);
    assert(tx.aux_tfds.size == IWM_TX_HW_QUEUE_SIZE * IWM_TX_TFD_SIZE);
    assert(hw.scd_txfact == 0); /* Linux keeps SCD stopped before ALIVE. */
    assert(iwm_tx_queue_set_ptr(&tx, IWM_TX_AUX_QUEUE, 0) == IWM_ESTATE);
    assert(iwm_tx_set_write_ptr(&tx, 1) == IWM_ESTATE);

    assert(iwm_tx_start_scheduler(&tx) == IWM_OK);
    assert(tx.scheduler_started && tx.scd_base == hw.scd_sram_base);
    assert(hw.scd_dram_base == tx.scd_bc_table.paddr >> 10);
    assert(hw.scd_txfact == 0xffU);
    assert(hw.scd_status0 == 0x017f001fUL);
    assert(hw.scd_rdptr0 == 0);
    assert(hw.mem_writes == 137U); /* 135 clear dwords + two queue context. */
    assert(hw.first_mem_addr == hw.scd_sram_base + 0x600U);
    assert(hw.last_mem_addr == hw.scd_sram_base + 0x604U);
    assert(hw.regs[FH_TCSR_CONFIG(0) >> 2] == 0x80000008UL);
    assert(hw.regs[FH_TCSR_CONFIG(7) >> 2] == 0x80000008UL);
    assert(hw.regs[FH_TX_CHICKEN_BITS_REG >> 2] & 2U);

    {
        /* NVM_ACCESS read of section 0: 4-byte header + 8-byte payload fits
         * the 20-byte first TB, so the TFD carries one TB. */
        static const uint8_t nvm[8] = { 0, 0, 0, 0, 0, 0, 0x00, 0x08 };
        uint8_t big[40];
        const uint8_t *tfd = (const uint8_t *)tx.cmd_tfds.vaddr;
        const uint8_t *first = (const uint8_t *)tx.cmd_first_tb.vaddr;
        uint16_t seq = 0xffff;
        uint32_t lo;
        assert(iwm_tx_send_cmd(&tx, 0x88, 0, nvm, 8, &seq) == IWM_OK);
        assert(seq == 0x0000 && tx.write == 1U);
        assert(hw.regs[HBUS_TARG_WRPTR >> 2] == 1U);
        assert(tfd[3] == 1U);
        lo = (uint32_t)tfd[4] | ((uint32_t)tfd[5] << 8) |
             ((uint32_t)tfd[6] << 16) | ((uint32_t)tfd[7] << 24);
        assert(lo == tx.cmd_first_tb.paddr);
        assert(((uint32_t)tfd[8] | ((uint32_t)tfd[9] << 8)) == (12U << 4));
        assert(first[0] == 0x88 && first[1] == 0 && first[2] == 0 &&
               first[3] == 0 && first[11] == 0x08);
        /* A 40-byte payload splits: 20 bytes in TB0, 24 in TB1. */
        memset(big, 0xa5, sizeof(big));
        assert(iwm_tx_send_cmd(&tx, 0x6a, 0, big, sizeof(big), &seq) == IWM_OK);
        assert(seq == 0x0001 && hw.regs[HBUS_TARG_WRPTR >> 2] == 2U);
        tfd += IWM_TX_TFD_SIZE;
        assert(tfd[3] == 2U);
        lo = (uint32_t)tfd[10] | ((uint32_t)tfd[11] << 8) |
             ((uint32_t)tfd[12] << 16) | ((uint32_t)tfd[13] << 24);
        assert(lo == tx.cmd_buffers.paddr + IWM_TX_CMD_BUFFER_SIZE + 20U);
        assert(((uint32_t)tfd[14] | ((uint32_t)tfd[15] << 8)) == (24U << 4));
        /* Wide header (group 1): cmd, group, seq, le16 len, rsvd, version. */
        {
            const uint8_t *c = (const uint8_t *)tx.cmd_buffers.vaddr +
                               2U * IWM_TX_CMD_BUFFER_SIZE;
            assert(iwm_tx_send_cmd(&tx, 0x4f, 1, nvm, 8, &seq) == IWM_OK);
            assert(seq == 0x0002 && hw.regs[HBUS_TARG_WRPTR >> 2] == 3U);
            assert(c[0] == 0x4f && c[1] == 1 && c[2] == 2 && c[3] == 0);
            assert(c[4] == 8 && c[5] == 0 && c[6] == 0 && c[7] == 0);
            assert(c[15] == 0x08);
            tfd = (const uint8_t *)tx.cmd_tfds.vaddr + 2U * IWM_TX_TFD_SIZE;
            assert(tfd[3] == 1U);
            assert(((uint32_t)tfd[8] | ((uint32_t)tfd[9] << 8)) == (16U << 4));
        }
        assert(iwm_tx_send_cmd(&tx, 0x6c, 0, big,
                               (uint16_t)(IWM_TX_CMD_MAX_PAYLOAD + 1U),
                               &seq) == IWM_EINVAL);
        iwm_tx_cmd_done(&tx, 1);
        assert(tx.read == 2U);
        tx.read = 0;
        tx.write = 0;
    }
    assert(iwm_tx_queue_set_ptr(&tx, IWM_TX_AUX_QUEUE, 0) == IWM_OK);
    assert(hw.regs[HBUS_TARG_WRPTR >> 2] == 0x100U);
    assert(iwm_tx_queue_set_ptr(&tx, IWM_TX_CMD_QUEUE, 0) == IWM_EINVAL);
    assert(iwm_tx_queue_set_ptr(&tx, IWM_TX_NUM_QUEUES, 0) == IWM_EINVAL);
    assert(iwm_tx_set_write_ptr(&tx, IWM_TX_HW_QUEUE_SIZE) == IWM_EINVAL);
    assert(iwm_tx_set_write_ptr(&tx, 17U) == IWM_OK);
    assert(hw.regs[HBUS_TARG_WRPTR >> 2] == 17U && tx.write == 17U);

    iwm_tx_debug_snapshot(&tx, &debug);
    assert(debug.allocated && debug.prepared && debug.scheduler_started);
    assert(debug.keep_warm == tx.keep_warm.paddr >> 4);
    assert(debug.cmd_cbbc == tx.cmd_tfds.paddr >> 8);
    assert(debug.scd_dram_base == tx.scd_bc_table.paddr >> 10);
    assert(debug.scd_cmd_status == 0x017f001fUL);

    iwm_tx_stop(&tx);
    assert(!tx.prepared && !tx.scheduler_started);
    assert(hw.regs[FH_KW_MEM_ADDR_REG >> 2] == 0);
    assert(hw.regs[FH_MEM_CBBC_QUEUE0 >> 2] == 0);
    assert(hw.regs[(FH_MEM_CBBC_QUEUE0 + 4U) >> 2] == 0);
    assert(hw.scd_txfact == 0);
    iwm_tx_free(&tx);
    assert(!tx.allocated && !tx.keep_warm.vaddr && !tx.cmd_tfds.vaddr);

    puts("tx transport tests passed");
    return 0;
}
