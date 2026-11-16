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
#define HBUS_TARG_MEM_WADDR      0x400U
#define HBUS_TARG_MEM_WDAT       0x404U
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
        return;
    }
    if (off == HBUS_TARG_PRPH_WDAT) {
        switch (m->prph_waddr) {
        case SCD_DRAM_BASE_ADDR: m->scd_dram_base = value; break;
        case SCD_TXFACT: m->scd_txfact = value; break;
        case SCD_QUEUE_RDPTR0: m->scd_rdptr0 = value; break;
        case SCD_QUEUE_STATUS0: m->scd_status0 = value; break;
        case SCD_AGGR_SEL: m->scd_aggr = value; break;
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

    saved_tfd_phys = tx.cmd_tfds.paddr;
    tx.cmd_tfds.paddr += 4U;
    assert(iwm_tx_prepare(&tx) == IWM_ERANGE);
    tx.cmd_tfds.paddr = saved_tfd_phys;

    assert(iwm_tx_prepare(&tx) == IWM_OK);
    assert(tx.prepared && !tx.scheduler_started);
    assert(hw.regs[FH_KW_MEM_ADDR_REG >> 2] == tx.keep_warm.paddr >> 4);
    assert(hw.regs[FH_MEM_CBBC_QUEUE0 >> 2] == tx.cmd_tfds.paddr >> 8);
    assert(hw.scd_txfact == 0); /* Linux keeps SCD stopped before ALIVE. */
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
    assert(hw.scd_txfact == 0);
    iwm_tx_free(&tx);
    assert(!tx.allocated && !tx.keep_warm.vaddr && !tx.cmd_tfds.vaddr);

    puts("tx transport tests passed");
    return 0;
}
