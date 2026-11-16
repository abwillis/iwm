/* SPDX-License-Identifier: GPL-2.0-only */
/* Intel 9000-family pre-22000 FH/SCD transport prerequisites. */
#if !defined(IWM_RING0)
#include <string.h>
#endif
#include "iwm_tx.h"
#include "iwm_transport.h"

#define HBUS_TARG_MEM_WADDR              0x400UL
#define HBUS_TARG_MEM_WDAT               0x404UL
#define HBUS_TARG_PRPH_WADDR             0x444UL
#define HBUS_TARG_PRPH_RADDR             0x448UL
#define HBUS_TARG_PRPH_WDAT              0x44cUL
#define HBUS_TARG_PRPH_RDAT              0x450UL
#define HBUS_TARG_WRPTR                  0x460UL
#define HBUS_PRPH_DWORD                  (3UL << 24)

#define FH_KW_MEM_ADDR_REG               0x197cUL
#define FH_MEM_CBBC_0_15                 0x19d0UL
#define FH_MEM_CBBC_16_19                0x1bf0UL
#define FH_MEM_CBBC_20_31                0x1b20UL
#define FH_TCSR_CONFIG(ch)               (0x1d00UL + 0x20UL * (ch))
#define FH_TX_CHICKEN_BITS_REG           0x1e98UL
#define FH_TX_CHICKEN_SCD_AUTO_RETRY     0x00000002UL
#define FH_TCSR_DMA_ENABLE               0x80000000UL
#define FH_TCSR_CREDIT_ENABLE            0x00000008UL

#define SCD_BASE                         0x00a02c00UL
#define SCD_SRAM_BASE_ADDR               (SCD_BASE + 0x000UL)
#define SCD_DRAM_BASE_ADDR               (SCD_BASE + 0x008UL)
#define SCD_TXFACT                       (SCD_BASE + 0x010UL)
#define SCD_QUEUE_WRPTR(q)               (SCD_BASE + 0x018UL + 4UL * (q))
#define SCD_QUEUE_RDPTR(q)               (SCD_BASE + 0x068UL + 4UL * (q))
#define SCD_QUEUE_STATUS(q)              (SCD_BASE + 0x10cUL + 4UL * (q))
#define SCD_AGGR_SEL                     (SCD_BASE + 0x248UL)
#define SCD_CONTEXT_MEM_LOWER_BOUND      0x600UL
#define SCD_TRANS_TBL_MEM_LOWER_BOUND    0x7e0UL
#define SCD_TRANS_TBL_OFFSET_QUEUE(q)    \
    ((SCD_TRANS_TBL_MEM_LOWER_BOUND + 2UL * (q)) & 0xfffffffcUL)
#define SCD_CONTEXT_QUEUE_OFFSET(q)      \
    (SCD_CONTEXT_MEM_LOWER_BOUND + 8UL * (q))
#define SCD_QUEUE_INACTIVE               0x00080000UL
#define SCD_QUEUE_ACTIVE_MASK            0x017f0000UL
#define SCD_QUEUE_ACTIVE                 0x00000008UL
#define SCD_QUEUE_WINDOW_SELECT          0x00000010UL
#define SCD_QUEUE_CTX_REG2               0x00400040UL
#define SCD_TXFACT_ALL_FIFOS             0x000000ffUL

static uint32_t rd(struct iwm_tx_transport *tx, uint32_t reg)
{
    return tx->adapter->ops->mmio_read32(tx->adapter->os_context, reg);
}

static void wr(struct iwm_tx_transport *tx, uint32_t reg, uint32_t value)
{
    tx->adapter->ops->mmio_write32(tx->adapter->os_context, reg, value);
}

static void prph_write_locked(struct iwm_tx_transport *tx, uint32_t reg,
                              uint32_t value)
{
    wr(tx, HBUS_TARG_PRPH_WADDR, reg | HBUS_PRPH_DWORD);
    wr(tx, HBUS_TARG_PRPH_WDAT, value);
}

static uint32_t prph_read_locked(struct iwm_tx_transport *tx, uint32_t reg)
{
    wr(tx, HBUS_TARG_PRPH_RADDR, reg | HBUS_PRPH_DWORD);
    return rd(tx, HBUS_TARG_PRPH_RDAT);
}

static void mem_write_locked(struct iwm_tx_transport *tx, uint32_t addr,
                             uint32_t value)
{
    wr(tx, HBUS_TARG_MEM_WADDR, addr);
    wr(tx, HBUS_TARG_MEM_WDAT, value);
}

static uint32_t cbbc_reg(unsigned queue)
{
    if (queue < 16U)
        return FH_MEM_CBBC_0_15 + 4UL * queue;
    if (queue < 20U)
        return FH_MEM_CBBC_16_19 + 4UL * (queue - 16U);
    return FH_MEM_CBBC_20_31 + 4UL * (queue - 20U);
}

static int alloc_one(struct iwm_tx_transport *tx, uint32_t size,
                     uint32_t alignment, struct iwm_dma_block *block)
{
    int rc;

    rc = tx->adapter->ops->dma_alloc(tx->adapter->os_context, size,
                                     alignment, block);
    if (rc != IWM_OK)
        return rc;
    rc = iwm_dma32_check(block->paddr, block->size, alignment);
    if (rc != IWM_OK) {
        tx->adapter->ops->dma_free(tx->adapter->os_context, block);
        memset(block, 0, sizeof(*block));
    }
    return rc;
}

void iwm_tx_init(struct iwm_tx_transport *tx, struct iwm_adapter *adapter)
{
    if (!tx)
        return;
    memset(tx, 0, sizeof(*tx));
    tx->adapter = adapter;
}

int iwm_tx_alloc(struct iwm_tx_transport *tx)
{
    int rc;

    if (!tx || !tx->adapter || !tx->adapter->ops ||
        !tx->adapter->ops->dma_alloc || !tx->adapter->ops->dma_free)
        return IWM_EINVAL;
    if (tx->allocated)
        return IWM_OK;

    rc = alloc_one(tx, IWM_TX_KEEP_WARM_SIZE,
                   IWM_TX_KEEP_WARM_ALIGNMENT, &tx->keep_warm);
    if (rc != IWM_OK)
        goto fail;
    rc = alloc_one(tx,
                   IWM_TX_NUM_QUEUES * IWM_TX_BC_ENTRIES *
                   IWM_TX_BC_ENTRY_SIZE,
                   IWM_TX_BC_ALIGNMENT, &tx->scd_bc_table);
    if (rc != IWM_OK)
        goto fail;
    rc = alloc_one(tx, IWM_TX_HW_QUEUE_SIZE * IWM_TX_TFD_SIZE,
                   IWM_TX_TFD_ALIGNMENT, &tx->cmd_tfds);
    if (rc != IWM_OK)
        goto fail;
    rc = alloc_one(tx, IWM_TX_CMD_WINDOW * IWM_TX_FIRST_TB_SIZE,
                   IWM_TX_FIRST_TB_SIZE, &tx->cmd_first_tb);
    if (rc != IWM_OK)
        goto fail;
    rc = alloc_one(tx, IWM_TX_CMD_WINDOW * IWM_TX_CMD_BUFFER_SIZE,
                   IWM_TX_FIRST_TB_SIZE, &tx->cmd_buffers);
    if (rc != IWM_OK)
        goto fail;

    tx->allocated = 1;
    return IWM_OK;

fail:
    iwm_tx_free(tx);
    return rc;
}

void iwm_tx_stop(struct iwm_tx_transport *tx)
{
    unsigned channel;

    if (!tx || !tx->adapter || !tx->adapter->ops ||
        !tx->adapter->ops->mmio_write32)
        return;
    if (tx->adapter->state >= IWM_ADAPTER_PROBED &&
        iwm_grab_nic_access(tx->adapter) == IWM_OK) {
        prph_write_locked(tx, SCD_TXFACT, 0);
        prph_write_locked(tx, SCD_QUEUE_STATUS(IWM_TX_CMD_QUEUE),
                          SCD_QUEUE_INACTIVE);
        for (channel = 0; channel < 8U; ++channel)
            wr(tx, FH_TCSR_CONFIG(channel), 0);
        wr(tx, cbbc_reg(IWM_TX_CMD_QUEUE), 0);
        wr(tx, FH_KW_MEM_ADDR_REG, 0);
        iwm_release_nic_access(tx->adapter);
    }
    tx->prepared = 0;
    tx->scheduler_started = 0;
    tx->read = 0;
    tx->write = 0;
}

void iwm_tx_free(struct iwm_tx_transport *tx)
{
    struct iwm_adapter *adapter;

    if (!tx || !tx->adapter || !tx->adapter->ops ||
        !tx->adapter->ops->dma_free)
        return;
    adapter = tx->adapter;
    if (tx->prepared || tx->scheduler_started)
        iwm_tx_stop(tx);
    if (tx->cmd_buffers.vaddr)
        adapter->ops->dma_free(adapter->os_context, &tx->cmd_buffers);
    if (tx->cmd_first_tb.vaddr)
        adapter->ops->dma_free(adapter->os_context, &tx->cmd_first_tb);
    if (tx->cmd_tfds.vaddr)
        adapter->ops->dma_free(adapter->os_context, &tx->cmd_tfds);
    if (tx->scd_bc_table.vaddr)
        adapter->ops->dma_free(adapter->os_context, &tx->scd_bc_table);
    if (tx->keep_warm.vaddr)
        adapter->ops->dma_free(adapter->os_context, &tx->keep_warm);
    iwm_tx_init(tx, adapter);
}

int iwm_tx_prepare(struct iwm_tx_transport *tx)
{
    unsigned queue;
    int rc;

    if (!tx || !tx->adapter || !tx->allocated)
        return IWM_ESTATE;
    if (tx->adapter->state != IWM_ADAPTER_FW_WAIT)
        return IWM_ESTATE;
    if ((tx->keep_warm.paddr & (IWM_TX_KEEP_WARM_ALIGNMENT - 1U)) ||
        (tx->scd_bc_table.paddr & (IWM_TX_BC_ALIGNMENT - 1U)) ||
        (tx->cmd_tfds.paddr & (IWM_TX_TFD_ALIGNMENT - 1U)))
        return IWM_ERANGE;

    memset(tx->keep_warm.vaddr, 0, tx->keep_warm.size);
    memset(tx->scd_bc_table.vaddr, 0, tx->scd_bc_table.size);
    memset(tx->cmd_tfds.vaddr, 0, tx->cmd_tfds.size);
    memset(tx->cmd_first_tb.vaddr, 0, tx->cmd_first_tb.size);
    memset(tx->cmd_buffers.vaddr, 0, tx->cmd_buffers.size);

    rc = iwm_grab_nic_access(tx->adapter);
    if (rc != IWM_OK)
        return rc;

    /* iwl_pcie_tx_init(): SCD remains stopped until firmware ALIVE. */
    prph_write_locked(tx, SCD_TXFACT, 0);
    wr(tx, FH_KW_MEM_ADDR_REG, tx->keep_warm.paddr >> 4);
    for (queue = 0; queue < IWM_TX_NUM_QUEUES; ++queue)
        wr(tx, cbbc_reg(queue), 0);
    wr(tx, cbbc_reg(IWM_TX_CMD_QUEUE), tx->cmd_tfds.paddr >> 8);
    wr(tx, HBUS_TARG_WRPTR, IWM_TX_CMD_QUEUE << 8);
    iwm_release_nic_access(tx->adapter);

    tx->read = 0;
    tx->write = 0;
    tx->prepared = 1;
    tx->scheduler_started = 0;
    return IWM_OK;
}

int iwm_tx_start_scheduler(struct iwm_tx_transport *tx)
{
    uint32_t clear_end;
    uint32_t address;
    uint32_t value;
    unsigned channel;
    int rc;

    if (!tx || !tx->adapter || !tx->prepared || !tx->allocated)
        return IWM_ESTATE;
    if (tx->scheduler_started)
        return IWM_OK;

    rc = iwm_grab_nic_access(tx->adapter);
    if (rc != IWM_OK)
        return rc;

    /* iwl_pcie_tx_start(): the SCD SRAM base is valid only after ALIVE. */
    tx->scd_base = prph_read_locked(tx, SCD_SRAM_BASE_ADDR);
    if (tx->scd_base == 0xffffffffUL) {
        iwm_release_nic_access(tx->adapter);
        return IWM_EIO;
    }
    clear_end = SCD_TRANS_TBL_OFFSET_QUEUE(IWM_TX_NUM_QUEUES);
    for (address = SCD_CONTEXT_MEM_LOWER_BOUND;
         address < clear_end; address += 4U)
        mem_write_locked(tx, tx->scd_base + address, 0);

    prph_write_locked(tx, SCD_DRAM_BASE_ADDR,
                      tx->scd_bc_table.paddr >> 10);
    prph_write_locked(tx, SCD_QUEUE_STATUS(IWM_TX_CMD_QUEUE),
                      SCD_QUEUE_INACTIVE);
    value = prph_read_locked(tx, SCD_AGGR_SEL);
    prph_write_locked(tx, SCD_AGGR_SEL,
                      value & ~(1UL << IWM_TX_CMD_QUEUE));
    wr(tx, HBUS_TARG_WRPTR, IWM_TX_CMD_QUEUE << 8);
    prph_write_locked(tx, SCD_QUEUE_RDPTR(IWM_TX_CMD_QUEUE), 0);
    mem_write_locked(tx,
        tx->scd_base + SCD_CONTEXT_QUEUE_OFFSET(IWM_TX_CMD_QUEUE), 0);
    mem_write_locked(tx,
        tx->scd_base + SCD_CONTEXT_QUEUE_OFFSET(IWM_TX_CMD_QUEUE) + 4U,
        SCD_QUEUE_CTX_REG2);
    prph_write_locked(tx, SCD_QUEUE_STATUS(IWM_TX_CMD_QUEUE),
                      SCD_QUEUE_ACTIVE_MASK | SCD_QUEUE_ACTIVE |
                      SCD_QUEUE_WINDOW_SELECT | IWM_TX_CMD_FIFO);
    prph_write_locked(tx, SCD_TXFACT, SCD_TXFACT_ALL_FIFOS);

    for (channel = 0; channel < 8U; ++channel)
        wr(tx, FH_TCSR_CONFIG(channel),
           FH_TCSR_DMA_ENABLE | FH_TCSR_CREDIT_ENABLE);
    value = rd(tx, FH_TX_CHICKEN_BITS_REG);
    wr(tx, FH_TX_CHICKEN_BITS_REG,
       value | FH_TX_CHICKEN_SCD_AUTO_RETRY);
    iwm_release_nic_access(tx->adapter);

    tx->scheduler_started = 1;
    return IWM_OK;
}

int iwm_tx_set_write_ptr(struct iwm_tx_transport *tx, uint16_t write_ptr)
{
    if (!tx || !tx->scheduler_started)
        return IWM_ESTATE;
    if (write_ptr >= IWM_TX_HW_QUEUE_SIZE)
        return IWM_EINVAL;
    wr(tx, HBUS_TARG_WRPTR,
       (uint32_t)write_ptr | ((uint32_t)IWM_TX_CMD_QUEUE << 8));
    tx->write = write_ptr;
    return IWM_OK;
}

void iwm_tx_debug_snapshot(struct iwm_tx_transport *tx,
                           struct iwm_tx_debug *debug)
{
    if (!debug)
        return;
    memset(debug, 0, sizeof(*debug));
    if (!tx)
        return;
    debug->allocated = tx->allocated;
    debug->prepared = tx->prepared;
    debug->scheduler_started = tx->scheduler_started;
    if (!tx->adapter || !tx->adapter->ops ||
        !tx->adapter->ops->mmio_read32 ||
        iwm_grab_nic_access(tx->adapter) != IWM_OK)
        return;

    debug->keep_warm = rd(tx, FH_KW_MEM_ADDR_REG);
    debug->cmd_cbbc = rd(tx, cbbc_reg(IWM_TX_CMD_QUEUE));
    debug->fh_config_0 = rd(tx, FH_TCSR_CONFIG(0));
    debug->fh_config_7 = rd(tx, FH_TCSR_CONFIG(7));
    debug->fh_chicken = rd(tx, FH_TX_CHICKEN_BITS_REG);
    debug->scd_sram_base = prph_read_locked(tx, SCD_SRAM_BASE_ADDR);
    debug->scd_dram_base = prph_read_locked(tx, SCD_DRAM_BASE_ADDR);
    debug->scd_txfact = prph_read_locked(tx, SCD_TXFACT);
    debug->scd_cmd_rdptr = prph_read_locked(tx,
                                            SCD_QUEUE_RDPTR(IWM_TX_CMD_QUEUE));
    debug->scd_cmd_wrptr = prph_read_locked(tx,
                                            SCD_QUEUE_WRPTR(IWM_TX_CMD_QUEUE));
    debug->scd_cmd_status = prph_read_locked(tx,
                                             SCD_QUEUE_STATUS(IWM_TX_CMD_QUEUE));
    debug->hbus_wrptr = rd(tx, HBUS_TARG_WRPTR);
    iwm_release_nic_access(tx->adapter);
}
