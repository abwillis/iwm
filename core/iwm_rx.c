/* SPDX-License-Identifier: GPL-2.0-only */
/* Intel 9000-family RFH receive queue 0 and ALIVE notification parser. */
#if !defined(IWM_RING0)
#include <string.h>
#endif
#include "iwm_rx.h"
#include "iwm_transport.h"

#define HBUS_TARG_PRPH_WADDR          0x444UL
#define HBUS_TARG_PRPH_RADDR          0x448UL
#define HBUS_TARG_PRPH_WDAT           0x44cUL
#define HBUS_TARG_PRPH_RDAT           0x450UL
#define HBUS_PRPH_DWORD               (3UL << 24)
#define RFH_Q0_FRBDCB_BA_LSB          0x00a08000UL
#define RFH_Q0_FRBDCB_WIDX            0x00a08080UL
#define RFH_Q0_FRBDCB_RIDX            0x00a080c0UL
#define RFH_Q0_URBDCB_BA_LSB          0x00a08100UL
#define RFH_Q0_URBDCB_WIDX            0x00a08180UL
#define RFH_Q0_URBD_STTS_WPTR_LSB     0x00a08200UL
#define RFH_RXF_DMA_CFG               0x00a09820UL
#define RFH_RXF_RXQ_ACTIVE            0x00a0980cUL
#define RFH_GEN_CFG                   0x00a09800UL
#define RFH_Q0_FRBDCB_WIDX_TRG        0x1c80UL
/* Linux iwl_pcie_nic_init(): shadow registers must be enabled before the
 * CSR shadow write-index trigger is forwarded to the RFH. */
#define CSR_MAC_SHADOW_REG_CTRL       0x0a8UL
#define CSR_MAC_SHADOW_REG_CTRL_ALL   0x800FFFFFUL
#define RFH_RXF_DMA_RB_SIZE_4K        (4UL << 16)
#define RFH_RXF_DMA_RBDCB_SIZE_512    (9UL << 20)
#define RFH_RXF_DMA_MIN_RB_4_8        (3UL << 24)
#define RFH_RXF_DMA_DROP_TOO_LARGE    0x04000000UL
#define RFH_DMA_ENABLE                0x80000000UL
#define RFH_GEN_SERVICE_DMA_SNOOP     0x00000001UL
#define RFH_GEN_RFH_DMA_SNOOP         0x00000002UL
#define RFH_RXQ0_ACTIVE               0x00010001UL
/* FreeBSD iwm_nic_rx_mq_init(): IWM_CSR_INT_COALESCING (byte at 0x004) =
 * IWM_HOST_INT_TIMEOUT_DEF. Byte 0x005 is the periodic-interrupt register,
 * so only the low byte is changed. */
#define CSR_INT_COALESCING            0x004UL
#define HOST_INT_TIMEOUT_DEF          0x40UL

static void wr(struct iwm_rx_queue *q, uint32_t reg, uint32_t value)
{
    q->adapter->ops->mmio_write32(q->adapter->os_context, reg, value);
}

static int prph_write_locked(struct iwm_rx_queue *q, uint32_t reg,
                             uint32_t value)
{
    wr(q, HBUS_TARG_PRPH_WADDR, reg | HBUS_PRPH_DWORD);
    wr(q, HBUS_TARG_PRPH_WDAT, value);
    return IWM_OK;
}

static uint32_t prph_read_locked(struct iwm_rx_queue *q, uint32_t reg)
{
    wr(q, HBUS_TARG_PRPH_RADDR, reg | HBUS_PRPH_DWORD);
    return q->adapter->ops->mmio_read32(q->adapter->os_context,
                                        HBUS_TARG_PRPH_RDAT);
}

void iwm_rx_debug(struct iwm_rx_queue *q, uint32_t out[5])
{
    volatile struct iwm_rx_status *s;
    unsigned i;

    for (i = 0; i < 5U; ++i)
        out[i] = 0;
    if (!q || !q->adapter || !q->adapter->ops ||
        !q->adapter->ops->mmio_read32 || !q->programmed)
        return;
    if (iwm_grab_nic_access(q->adapter) == IWM_OK) {
        out[0] = prph_read_locked(q, RFH_RXF_DMA_CFG);
        out[1] = prph_read_locked(q, RFH_RXF_RXQ_ACTIVE);
        out[2] = prph_read_locked(q, RFH_Q0_FRBDCB_WIDX);
        out[3] = prph_read_locked(q, RFH_Q0_FRBDCB_RIDX);
        iwm_release_nic_access(q->adapter);
    }
    if (q->status.vaddr) {
        s = (volatile struct iwm_rx_status *)q->status.vaddr;
        out[4] = (uint32_t)s->closed_rb_num |
                 ((uint32_t)s->finished_rb_num << 16);
    }
}

static void desc_set(struct iwm_rx_queue *q, uint16_t slot, uint16_t rbid)
{
    uint32_t *d = (uint32_t *)q->free_desc.vaddr;
    uint32_t addr = q->buffers.paddr + (uint32_t)rbid * IWM_RX_BUFFER_SIZE;

    /* Pre-AX210 MQ descriptors are little-endian 64-bit values containing a
     * 4 KiB-aligned DMA address and the virtual buffer ID in the low 12 bits.
     * Linux numbers buffer IDs from 1 (rxb->vid = i + 1) and treats 0 as
     * invalid, so buffer n is posted as ID n + 1. */
    d[(uint32_t)slot * 2U] = addr | ((uint32_t)rbid + 1U);
    d[(uint32_t)slot * 2U + 1U] = 0;
}

static int alloc_one(struct iwm_rx_queue *q, uint32_t size,
                     uint32_t alignment, struct iwm_dma_block *block)
{
    int rc = q->adapter->ops->dma_alloc(q->adapter->os_context, size,
                                        alignment, block);
    if (rc != IWM_OK)
        return rc;
    rc = iwm_dma32_check(block->paddr, block->size, alignment);
    if (rc != IWM_OK) {
        q->adapter->ops->dma_free(q->adapter->os_context, block);
        memset(block, 0, sizeof(*block));
    }
    return rc;
}

void iwm_rx_init(struct iwm_rx_queue *q, struct iwm_adapter *a)
{
    if (!q)
        return;
    memset(q, 0, sizeof(*q));
    q->adapter = a;
}

int iwm_rx_alloc(struct iwm_rx_queue *q)
{
    uint16_t i;
    int rc;

    if (!q || !q->adapter || !q->adapter->ops ||
        !q->adapter->ops->dma_alloc || !q->adapter->ops->dma_free)
        return IWM_EINVAL;
    if (q->buffers.vaddr)
        return IWM_OK;

    rc = alloc_one(q, IWM_RX_QUEUE_SIZE * 8U, IWM_RX_RBD_ALIGNMENT,
                   &q->free_desc);
    if (rc != IWM_OK)
        goto fail;
    rc = alloc_one(q, IWM_RX_QUEUE_SIZE * 4U, IWM_RX_RBD_ALIGNMENT,
                   &q->used_desc);
    if (rc != IWM_OK)
        goto fail;
    rc = alloc_one(q, sizeof(struct iwm_rx_status),
                   IWM_RX_STATUS_ALIGNMENT, &q->status);
    if (rc != IWM_OK)
        goto fail;
    rc = alloc_one(q, IWM_RX_QUEUE_SIZE * IWM_RX_BUFFER_SIZE,
                   IWM_RX_RBD_ALIGNMENT, &q->buffers);
    if (rc != IWM_OK)
        goto fail;

    for (i = 0; i < IWM_RX_POSTED_COUNT; ++i)
        desc_set(q, i, i);
    q->read = 0;
    q->posted = IWM_RX_POSTED_COUNT;
    q->write_actual = IWM_RX_POSTED_COUNT & ~7U;
    return IWM_OK;

fail:
    iwm_rx_free(q);
    return rc;
}

void iwm_rx_free(struct iwm_rx_queue *q)
{
    struct iwm_adapter *a;

    if (!q || !q->adapter || !q->adapter->ops || !q->adapter->ops->dma_free)
        return;
    a = q->adapter;
    if (q->buffers.vaddr)
        a->ops->dma_free(a->os_context, &q->buffers);
    if (q->status.vaddr)
        a->ops->dma_free(a->os_context, &q->status);
    if (q->used_desc.vaddr)
        a->ops->dma_free(a->os_context, &q->used_desc);
    if (q->free_desc.vaddr)
        a->ops->dma_free(a->os_context, &q->free_desc);
    iwm_rx_init(q, a);
}

int iwm_rx_program(struct iwm_rx_queue *q)
{
    uint16_t i;
    int rc;

    if (!q || !q->adapter || !q->free_desc.vaddr || !q->used_desc.vaddr ||
        !q->status.vaddr || !q->buffers.vaddr)
        return IWM_ESTATE;
    if ((q->free_desc.paddr & 0xfffU) || (q->used_desc.paddr & 0xfffU) ||
        (q->buffers.paddr & 0xfffU))
        return IWM_ERANGE;

    memset(q->used_desc.vaddr, 0, q->used_desc.size);
    memset(q->status.vaddr, 0, q->status.size);
    memset(q->buffers.vaddr, 0, q->buffers.size);
    for (i = 0; i < IWM_RX_POSTED_COUNT; ++i)
        desc_set(q, i, i);
    q->read = 0;
    q->posted = IWM_RX_POSTED_COUNT;
    q->write_actual = IWM_RX_POSTED_COUNT & ~7U;
    q->last_closed = 0;
    q->packets = 0;
    q->malformed = 0;
    q->alive_seen = 0;
    memset(&q->alive, 0, sizeof(q->alive));

    rc = iwm_grab_nic_access(q->adapter);
    if (rc != IWM_OK)
        return rc;
    prph_write_locked(q, RFH_RXF_DMA_CFG, 0);
    prph_write_locked(q, RFH_RXF_RXQ_ACTIVE, 0);
    prph_write_locked(q, RFH_Q0_FRBDCB_BA_LSB, q->free_desc.paddr);
    prph_write_locked(q, RFH_Q0_FRBDCB_BA_LSB + 4U, 0);
    prph_write_locked(q, RFH_Q0_URBDCB_BA_LSB, q->used_desc.paddr);
    prph_write_locked(q, RFH_Q0_URBDCB_BA_LSB + 4U, 0);
    prph_write_locked(q, RFH_Q0_URBD_STTS_WPTR_LSB, q->status.paddr);
    prph_write_locked(q, RFH_Q0_URBD_STTS_WPTR_LSB + 4U, 0);
    prph_write_locked(q, RFH_Q0_FRBDCB_WIDX, 0);
    prph_write_locked(q, RFH_Q0_FRBDCB_RIDX, 0);
    prph_write_locked(q, RFH_Q0_URBDCB_WIDX, 0);
    prph_write_locked(q, RFH_RXF_DMA_CFG,
                      RFH_DMA_ENABLE | RFH_RXF_DMA_RB_SIZE_4K |
                      RFH_RXF_DMA_MIN_RB_4_8 |
                      RFH_RXF_DMA_DROP_TOO_LARGE |
                      RFH_RXF_DMA_RBDCB_SIZE_512);
    prph_write_locked(q, RFH_GEN_CFG,
                      RFH_GEN_RFH_DMA_SNOOP | RFH_GEN_SERVICE_DMA_SNOOP);
    prph_write_locked(q, RFH_RXF_RXQ_ACTIVE, RFH_RXQ0_ACTIVE);
    wr(q, CSR_INT_COALESCING,
       (q->adapter->ops->mmio_read32(q->adapter->os_context,
                                     CSR_INT_COALESCING) & ~0xffUL) |
       HOST_INT_TIMEOUT_DEF);
    iwm_release_nic_access(q->adapter);

    /* Enable shadow registers, then use the CSR shadow write-index trigger. */
    wr(q, CSR_MAC_SHADOW_REG_CTRL,
       q->adapter->ops->mmio_read32(q->adapter->os_context,
                                    CSR_MAC_SHADOW_REG_CTRL) |
       CSR_MAC_SHADOW_REG_CTRL_ALL);
    wr(q, RFH_Q0_FRBDCB_WIDX_TRG, q->write_actual);
    /* Verify the RFH sees the free buffers; without them firmware has
     * nowhere to put ALIVE (log: "dbg trg 00000000"). If the trigger was
     * dropped, write the index directly, as the hw init already does for 0. */
    q->widx_readback = 0xFFFFFFFFUL;
    q->widx_direct = 0xFFFFFFFFUL;
    if (iwm_grab_nic_access(q->adapter) == IWM_OK) {
        q->widx_readback = prph_read_locked(q, RFH_Q0_FRBDCB_WIDX);
        if (q->widx_readback != q->write_actual) {
            prph_write_locked(q, RFH_Q0_FRBDCB_WIDX, q->write_actual);
            q->widx_direct = prph_read_locked(q, RFH_Q0_FRBDCB_WIDX);
        }
        iwm_release_nic_access(q->adapter);
    }
    q->programmed = 1;
    return IWM_OK;
}

int iwm_rx_process_buffer(struct iwm_rx_queue *q,
                          const uint8_t *buf, uint32_t capacity)
{
    uint32_t frame_len, payload_len;
    const uint8_t *payload;

    if (!q || !buf || capacity < 8U)
        return IWM_EINVAL;
    frame_len = iwm_get_le32(buf) & IWM_RX_FRAME_SIZE_MASK;
    if (frame_len < 4U || frame_len > capacity - 4U) {
        ++q->malformed;
        return IWM_EFORMAT;
    }
    q->last_cmd = buf[4];
    q->last_group = buf[5];
    q->last_seq = iwm_get_le16(buf + 6U);
    payload = buf + 8U;
    payload_len = frame_len - 4U;
    ++q->packets;
    if (q->last_seq & IWM_RX_SEQ_RX_FRAME)
        ++q->notifications;
    else if (q->resp_wait && !q->resp_seen && q->last_seq == q->resp_seq) {
        q->resp_cmd = q->last_cmd;
        q->resp_group = q->last_group;
        q->resp_len = (uint16_t)payload_len;
        memset(q->resp, 0, sizeof(q->resp));
        memcpy(q->resp, payload, IWM_MIN(payload_len, IWM_RX_RESP_COPY));
        if (q->resp_buf && q->resp_cap)
            memcpy(q->resp_buf, payload,
                   IWM_MIN(payload_len, (uint32_t)q->resp_cap));
        q->resp_seen = 1;
        return IWM_OK;
    }
    if (q->notif_cb)
        q->notif_cb(q->notif_ctx, q->last_cmd, q->last_group, q->last_seq,
                    payload, payload_len);

    if (q->last_cmd != IWM_RX_CMD_ALIVE || q->last_group != 0)
        return IWM_OK;
    if (payload_len < 4U) {
        ++q->malformed;
        return IWM_EFORMAT;
    }
    q->alive.status = iwm_get_le16(payload);
    q->alive.flags = iwm_get_le16(payload + 2U);
    if (payload_len >= 12U) {
        q->alive.lmac_major = iwm_get_le32(payload + 4U);
        q->alive.lmac_minor = iwm_get_le32(payload + 8U);
    }
    if (q->alive.status != IWM_RX_ALIVE_STATUS_OK)
        return IWM_EIO;
    q->alive.valid = 1;
    q->alive_seen = 1;
    return IWM_OK;
}

int iwm_rx_drain(struct iwm_rx_queue *q)
{
    volatile struct iwm_rx_status *s;
    volatile uint32_t *used;
    uint16_t closed;
    int first_error = IWM_OK;

    if (!q || !q->programmed || !q->status.vaddr || !q->used_desc.vaddr)
        return IWM_ESTATE;
    s = (volatile struct iwm_rx_status *)q->status.vaddr;
    used = (volatile uint32_t *)q->used_desc.vaddr;

    /* A notification callback may send a firmware command and wait for its
     * response, which drains this queue again from inside the callback (the
     * association layer does: iwm_assoc_rx -> as_up -> iwm_sta_up -> ~20
     * commands).  Two rules make that safe.  A slot is consumed (q->read
     * advanced) BEFORE its callback runs, so the inner drain carries on with
     * the next buffer instead of delivering the same one again.  The
     * producer index is re-read on every pass, so this loop never works from
     * a stale value after an inner drain consumed more.
     * The old loop did neither: after one inner drain it ran on through the
     * whole 512-slot ring.  d27 log: every association "ignored" 19-38
     * frames, one TX response (seq 0500) was logged three times, "dataq resp"
     * reached 26 after 6 transmissions, and the stale TX responses moved the
     * queue-4 read index so that the next frame failed with rc -4. */
    if (q->draining)
        ++q->nested_drains;
    ++q->draining;
    for (;;) {
        uint32_t vid;
        uint16_t rbid;
        uint8_t *buf;
        int rc;
        closed = s->closed_rb_num & IWM_RX_QUEUE_MASK;
        q->last_closed = closed;
        if (q->read == closed)
            break;
        vid = used[q->read] & 0x0fffUL;
        q->read = (q->read + 1U) & IWM_RX_QUEUE_MASK;
        if (vid == 0 || vid > IWM_RX_QUEUE_SIZE) {
            /* No buffer to recycle; count it and move on. */
            ++q->malformed;
            if (first_error == IWM_OK)
                first_error = IWM_EFORMAT;
            continue;
        }
        rbid = (uint16_t)(vid - 1U);
        buf = (uint8_t *)q->buffers.vaddr +
              (uint32_t)rbid * IWM_RX_BUFFER_SIZE;
        rc = iwm_rx_process_buffer(q, buf, IWM_RX_BUFFER_SIZE);
        if (rc != IWM_OK && first_error == IWM_OK)
            first_error = rc;
        desc_set(q, q->posted, rbid);
        q->posted = (q->posted + 1U) & IWM_RX_QUEUE_MASK;
    }
    --q->draining;
    if (q->write_actual != (q->posted & ~7U)) {
        q->write_actual = q->posted & ~7U;
        wr(q, RFH_Q0_FRBDCB_WIDX_TRG, q->write_actual);
    }
    return first_error;
}

int iwm_rx_wait_resp(struct iwm_rx_queue *q, uint16_t seq,
                     uint32_t timeout_us)
{
    uint32_t elapsed;
    if (!q || !q->adapter || !q->adapter->ops ||
        !q->adapter->ops->delay_us || !q->programmed)
        return IWM_ESTATE;
    q->resp_seq = seq;
    q->resp_seen = 0;
    q->resp_wait = 1;
    for (elapsed = 0; elapsed < timeout_us; elapsed += 100U) {
        int rc = iwm_rx_drain(q);
        if (q->resp_seen) {
            q->resp_wait = 0;
            return IWM_OK;
        }
        if (rc != IWM_OK && rc != IWM_EFORMAT && rc != IWM_EIO) {
            q->resp_wait = 0;
            return rc;
        }
        q->adapter->ops->delay_us(q->adapter->os_context, 100U);
    }
    q->resp_wait = 0;
    return IWM_ETIMEDOUT;
}

int iwm_rx_wait_alive(struct iwm_rx_queue *q, uint32_t timeout_us)
{
    uint32_t elapsed;

    if (!q || !q->adapter || !q->adapter->ops ||
        !q->adapter->ops->delay_us || !q->programmed)
        return IWM_ESTATE;
    for (elapsed = 0; elapsed < timeout_us; elapsed += 100U) {
        int rc = iwm_rx_drain(q);
        if (q->alive_seen)
            return q->alive.valid ? IWM_OK : IWM_EIO;
        if (rc != IWM_OK && rc != IWM_EFORMAT)
            return rc;
        q->adapter->ops->delay_us(q->adapter->os_context, 100U);
    }
    return IWM_ETIMEDOUT;
}
