/* SPDX-License-Identifier: GPL-2.0-only */
/* Intel 9000-family pre-22000 FH/SCD transport prerequisites. */
#if !defined(IWM_RING0)
#include <string.h>
#endif
#include "iwm_tx.h"
#include "iwm_transport.h"

/* Linux iwl-csr.h: HBUS_BASE 0x400; MEM_WADDR +0x10, MEM_WDAT +0x18. */
#define HBUS_TARG_MEM_WADDR              0x410UL
#define HBUS_TARG_MEM_WDAT               0x418UL
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
#define SCD_GP_CTRL                      (SCD_BASE + 0x1a8UL)
#define SCD_EN_CTRL                      (SCD_BASE + 0x254UL)
#define SCD_GP_CTRL_ENABLE_31_QUEUES     0x00000001UL
#define SCD_GP_CTRL_AUTO_ACTIVE_MODE     0x00040000UL
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
    rc = alloc_one(tx, IWM_TX_HW_QUEUE_SIZE * IWM_TX_TFD_SIZE,
                   IWM_TX_TFD_ALIGNMENT, &tx->aux_tfds);
    if (rc != IWM_OK)
        goto fail;
    {
        unsigned i;
        for (i = 0; i < IWM_TX_FRAME_QUEUES; ++i) {
            struct iwm_tx_fq *q = &tx->fq[i];
            q->queue = (uint8_t)(i == 0 ? IWM_TX_DATA_QUEUE : IWM_TX_MGMT_QUEUE);
            rc = alloc_one(tx, IWM_TX_HW_QUEUE_SIZE * IWM_TX_TFD_SIZE,
                           IWM_TX_TFD_ALIGNMENT, &q->tfds);
            if (rc == IWM_OK)
                rc = alloc_one(tx, IWM_TX_FRAME_WINDOW * IWM_TX_FIRST_TB_SIZE,
                               IWM_TX_FIRST_TB_SIZE, &q->first_tb);
            if (rc == IWM_OK)
                rc = alloc_one(tx, IWM_TX_FRAME_WINDOW * IWM_TX_FRAME_BUF_SIZE,
                               IWM_TX_FIRST_TB_SIZE, &q->bufs);
            if (rc != IWM_OK)
                goto fail;
        }
    }
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
        wr(tx, cbbc_reg(IWM_TX_AUX_QUEUE), 0);
        wr(tx, cbbc_reg(IWM_TX_DATA_QUEUE), 0);
        wr(tx, cbbc_reg(IWM_TX_MGMT_QUEUE), 0);
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
    {
        unsigned i;
        for (i = 0; i < IWM_TX_FRAME_QUEUES; ++i) {
            struct iwm_tx_fq *q = &tx->fq[i];
            if (q->bufs.vaddr)
                adapter->ops->dma_free(adapter->os_context, &q->bufs);
            if (q->first_tb.vaddr)
                adapter->ops->dma_free(adapter->os_context, &q->first_tb);
            if (q->tfds.vaddr)
                adapter->ops->dma_free(adapter->os_context, &q->tfds);
        }
    }
    if (tx->aux_tfds.vaddr)
        adapter->ops->dma_free(adapter->os_context, &tx->aux_tfds);
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
        (tx->cmd_tfds.paddr & (IWM_TX_TFD_ALIGNMENT - 1U)) ||
        (tx->aux_tfds.paddr & (IWM_TX_TFD_ALIGNMENT - 1U)) ||
        (tx->fq[0].tfds.paddr & (IWM_TX_TFD_ALIGNMENT - 1U)) ||
        (tx->fq[1].tfds.paddr & (IWM_TX_TFD_ALIGNMENT - 1U)))
        return IWM_ERANGE;

    memset(tx->keep_warm.vaddr, 0, tx->keep_warm.size);
    memset(tx->scd_bc_table.vaddr, 0, tx->scd_bc_table.size);
    memset(tx->cmd_tfds.vaddr, 0, tx->cmd_tfds.size);
    memset(tx->aux_tfds.vaddr, 0, tx->aux_tfds.size);
    {
        unsigned i;
        for (i = 0; i < IWM_TX_FRAME_QUEUES; ++i) {
            memset(tx->fq[i].tfds.vaddr, 0, tx->fq[i].tfds.size);
            tx->fq[i].read = 0;
            tx->fq[i].write = 0;
        }
    }
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
    /* Linux iwl_pcie_tx_init() publishes every allocated queue's ring; the
     * aux queue must not point at physical 0 once firmware enables it. */
    wr(tx, cbbc_reg(IWM_TX_AUX_QUEUE), tx->aux_tfds.paddr >> 8);
    /* Linux capture: CBBC for queues 4 and 5 published at every load. */
    wr(tx, cbbc_reg(IWM_TX_DATA_QUEUE), tx->fq[0].tfds.paddr >> 8);
    wr(tx, cbbc_reg(IWM_TX_MGMT_QUEUE), tx->fq[1].tfds.paddr >> 8);
    /* Linux iwl_pcie_tx_init(): auto-active mode, 31-queue scheduler. */
    prph_write_locked(tx, SCD_GP_CTRL,
                      prph_read_locked(tx, SCD_GP_CTRL) |
                      SCD_GP_CTRL_AUTO_ACTIVE_MODE |
                      SCD_GP_CTRL_ENABLE_31_QUEUES);
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
    /* Linux trace on this card: SCD_EN_CTRL = 0 before queue 0 is
     * configured, then BIT(0) once it is active. */
    prph_write_locked(tx, SCD_EN_CTRL, 0);
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
    prph_write_locked(tx, SCD_EN_CTRL, 1UL << IWM_TX_CMD_QUEUE);
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

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Legacy TFD (Linux struct iwl_tfd): 3 reserved bytes, num_tbs, then up to
 * 20 TBs of { le32 lo; le16 hi_n_len } where hi_n_len = len << 4 | addr 35:32. */
static void tfd_set_tb(uint8_t *tfd, unsigned n, uint32_t addr, uint32_t len)
{
    uint8_t *tb = tfd + 4U + 6U * n;
    put_le32(tb, addr);
    put_le16(tb + 4, (uint16_t)(len << 4));
}

int iwm_tx_send_cmd(struct iwm_tx_transport *tx, uint8_t cmd, uint8_t group,
                    const void *payload, uint16_t length, uint16_t *seq)
{
    uint16_t idx, next, slot, sequence;
    uint32_t total, tb0, hdr;
    uint8_t *buf, *first, *tfd;
    unsigned tbs;
    if (!tx || !tx->allocated || !tx->scheduler_started)
        return IWM_ESTATE;
    if (length > IWM_TX_CMD_MAX_PAYLOAD || (length && !payload))
        return IWM_EINVAL;
    idx = tx->write;
    next = (uint16_t)((idx + 1U) & (IWM_TX_HW_QUEUE_SIZE - 1U));
    if (next == tx->read)
        return IWM_EBUSY;
    slot = (uint16_t)(idx & (IWM_TX_CMD_WINDOW - 1U));
    /* Linux QUEUE_TO_SEQ(q) | INDEX_TO_SEQ(idx). */
    sequence = (uint16_t)(((IWM_TX_CMD_QUEUE & 0x1fU) << 8) | (idx & 0xffU));
    buf = (uint8_t *)tx->cmd_buffers.vaddr +
          (uint32_t)slot * IWM_TX_CMD_BUFFER_SIZE;
    hdr = group ? IWM_TX_CMD_HDR_WIDE_SIZE : IWM_TX_CMD_HDR_SIZE;
    buf[0] = cmd;
    buf[1] = group;
    put_le16(buf + 2, sequence);
    if (group) {
        put_le16(buf + 4, length);   /* payload length, header excluded */
        buf[6] = 0;                  /* reserved */
        buf[7] = 0;                  /* command version */
    }
    if (length)
        memcpy(buf + hdr, payload, length);
    total = hdr + (uint32_t)length;
    tb0 = IWM_MIN(total, IWM_TX_FIRST_TB_COPY);
    first = (uint8_t *)tx->cmd_first_tb.vaddr +
            (uint32_t)slot * IWM_TX_FIRST_TB_SIZE;
    memcpy(first, buf, tb0);
    tfd = (uint8_t *)tx->cmd_tfds.vaddr + (uint32_t)idx * IWM_TX_TFD_SIZE;
    memset(tfd, 0, IWM_TX_TFD_SIZE);
    tfd_set_tb(tfd, 0, tx->cmd_first_tb.paddr +
               (uint32_t)slot * IWM_TX_FIRST_TB_SIZE, tb0);
    tbs = 1;
    if (total > tb0) {
        tfd_set_tb(tfd, 1, tx->cmd_buffers.paddr +
                   (uint32_t)slot * IWM_TX_CMD_BUFFER_SIZE + tb0,
                   total - tb0);
        tbs = 2;
    }
    tfd[3] = (uint8_t)tbs;
    tx->write = next;
    /* Linux trace on this card: HBUS_TARG_WRPTR = (queue << 8) | new write
     * index, written without NIC access (shadow registers enabled). */
    wr(tx, HBUS_TARG_WRPTR, (uint32_t)next | ((uint32_t)IWM_TX_CMD_QUEUE << 8));
    if (seq)
        *seq = sequence;
    return IWM_OK;
}

int iwm_tx_queue_set_ptr(struct iwm_tx_transport *tx, unsigned queue,
                         uint16_t ptr)
{
    if (!tx || !tx->scheduler_started)
        return IWM_ESTATE;
    if (queue == IWM_TX_CMD_QUEUE || queue >= IWM_TX_NUM_QUEUES ||
        ptr >= IWM_TX_HW_QUEUE_SIZE)
        return IWM_EINVAL;
    wr(tx, HBUS_TARG_WRPTR, (uint32_t)ptr | ((uint32_t)queue << 8));
    return IWM_OK;
}

struct iwm_tx_fq *iwm_tx_fq_get(struct iwm_tx_transport *tx, unsigned queue)
{
    if (!tx)
        return NULL;
    if (queue == IWM_TX_DATA_QUEUE)
        return &tx->fq[0];
    if (queue == IWM_TX_MGMT_QUEUE)
        return &tx->fq[1];
    return NULL;
}

int iwm_tx_frame_queue_reset(struct iwm_tx_transport *tx, unsigned queue)
{
    struct iwm_tx_fq *q = iwm_tx_fq_get(tx, queue);
    if (!q)
        return IWM_EINVAL;
    q->read = 0;
    q->write = 0;
    if (q->tfds.vaddr)
        memset(q->tfds.vaddr, 0, q->tfds.size);
    return IWM_OK;
}

int iwm_tx_send_frame(struct iwm_tx_transport *tx, unsigned queue,
                      const uint8_t *txcmd, const uint8_t *frame,
                      uint16_t length, uint16_t hdr_len, uint16_t *seq)
{
    struct iwm_tx_fq *q = iwm_tx_fq_get(tx, queue);
    uint16_t idx, next, slot, sequence, pad, bc;
    uint32_t total, tb0, first_pa, off;
    uint8_t *buf, *tc, *tfd, *bct;
    if (!q || !tx->allocated || !tx->scheduler_started || !q->bufs.vaddr)
        return q ? IWM_ESTATE : IWM_EINVAL;
    if (!txcmd || !frame || hdr_len < 24U || hdr_len > length)
        return IWM_EINVAL;
    pad = (uint16_t)((4U - (hdr_len & 3U)) & 3U);
    total = IWM_TX_CMD_HDR_SIZE + IWM_TX_TXCMD_SIZE + (uint32_t)length + pad;
    if (total > IWM_TX_FRAME_BUF_SIZE)
        return IWM_EOVERFLOW;
    idx = q->write;
    next = (uint16_t)((idx + 1U) & (IWM_TX_HW_QUEUE_SIZE - 1U));
    if (((uint16_t)(idx - q->read) & (IWM_TX_HW_QUEUE_SIZE - 1U)) >=
        IWM_TX_FRAME_WINDOW - 1U)
        return IWM_EBUSY;
    slot = (uint16_t)(idx & (IWM_TX_FRAME_WINDOW - 1U));
    sequence = (uint16_t)(((queue & 0x1fU) << 8) | (idx & 0xffU));
    buf = (uint8_t *)q->bufs.vaddr + (uint32_t)slot * IWM_TX_FRAME_BUF_SIZE;
    first_pa = q->first_tb.paddr + (uint32_t)slot * IWM_TX_FIRST_TB_SIZE;
    buf[0] = IWM_TX_CMD_TX;
    buf[1] = 0;
    put_le16(buf + 2, sequence);
    tc = buf + IWM_TX_CMD_HDR_SIZE;
    memcpy(tc, txcmd, IWM_TX_TXCMD_SIZE);
    put_le16(tc + IWM_TX_TXCMD_LEN_OFF, length);
    put_le16(tc + IWM_TX_TXCMD_OFFLOAD_OFF,
             (uint16_t)(((uint32_t)(hdr_len / 2U) << IWM_TX_OFFLOAD_MH_SHIFT) |
                        (pad ? IWM_TX_OFFLOAD_MH_PAD : 0U)));
    /* Linux: scratch = TB0 + command header + offsetof(iwl_tx_cmd, scratch). */
    put_le32(tc + IWM_TX_TXCMD_DRAM_LSB_OFF, first_pa + IWM_TX_CMD_HDR_SIZE + 8U);
    tc[IWM_TX_TXCMD_DRAM_MSB_OFF] = 0;
    off = IWM_TX_CMD_HDR_SIZE + IWM_TX_TXCMD_SIZE;
    memcpy(buf + off, frame, hdr_len);
    off += hdr_len;
    memset(buf + off, 0, pad);
    off += pad;
    memcpy(buf + off, frame + hdr_len, (uint32_t)length - hdr_len);
    tb0 = IWM_MIN(total, IWM_TX_FIRST_TB_COPY);
    memcpy((uint8_t *)q->first_tb.vaddr + (uint32_t)slot * IWM_TX_FIRST_TB_SIZE,
           buf, tb0);
    tfd = (uint8_t *)q->tfds.vaddr + (uint32_t)idx * IWM_TX_TFD_SIZE;
    memset(tfd, 0, IWM_TX_TFD_SIZE);
    tfd_set_tb(tfd, 0, first_pa, tb0);
    tfd_set_tb(tfd, 1, q->bufs.paddr + (uint32_t)slot * IWM_TX_FRAME_BUF_SIZE + tb0,
               total - tb0);
    tfd[3] = 2;
    /* Linux iwl_pcie_txq_update_byte_cnt_tbl(), bc_table_dword: frame length
     * plus CRC and delimiter, in dwords, station in bits 15:12; entries below
     * 64 are mirrored at 256 + index. */
    bc = (uint16_t)((((uint32_t)length + 8U + 3U) / 4U) |
                    ((uint32_t)(txcmd[IWM_TX_TXCMD_STA_OFF] & 0xfU) << 12));
    bct = (uint8_t *)tx->scd_bc_table.vaddr +
          (uint32_t)queue * IWM_TX_BC_ENTRIES * IWM_TX_BC_ENTRY_SIZE;
    put_le16(bct + 2U * idx, bc);
    if (idx < 64U)
        put_le16(bct + 2U * (IWM_TX_HW_QUEUE_SIZE + idx), bc);
    q->write = next;
    ++q->sent;
    wr(tx, HBUS_TARG_WRPTR, (uint32_t)next | ((uint32_t)queue << 8));
    if (seq)
        *seq = sequence;
    return IWM_OK;
}

void iwm_tx_frame_done(struct iwm_tx_transport *tx, uint16_t seq)
{
    struct iwm_tx_fq *q = iwm_tx_fq_get(tx, (seq >> 8) & 0x1fU);
    uint16_t inflight, off;
    if (!q)
        return;
    /* Only a slot that is in flight (read <= index < write) can be reclaimed.
     * d27 log: three stale TX responses (seq 0402..0404, replayed by the
     * re-entrant RX drain) arrived after the queue had been reset, moved
     * read to 5 while write was 0, and the window check then refused every
     * frame with rc -4 (EBUSY).  A response outside the window is counted
     * and ignored. */
    inflight = (uint16_t)((q->write - q->read) & (IWM_TX_HW_QUEUE_SIZE - 1U));
    off = (uint16_t)(((seq & 0xffU) - q->read) & (IWM_TX_HW_QUEUE_SIZE - 1U));
    if (off >= inflight) {
        ++q->stale;
        return;
    }
    q->read = (uint16_t)(((seq & 0xffU) + 1U) & (IWM_TX_HW_QUEUE_SIZE - 1U));
    ++q->done;
}

void iwm_tx_cmd_done(struct iwm_tx_transport *tx, uint16_t seq)
{
    if (!tx)
        return;
    tx->read = (uint16_t)(((seq & 0xffU) + 1U) & (IWM_TX_HW_QUEUE_SIZE - 1U));
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
