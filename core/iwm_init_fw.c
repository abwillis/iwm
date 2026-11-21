/* SPDX-License-Identifier: GPL-2.0-only */
/* INIT firmware command sequence for the 9000 family (milestone B). */
#if !defined(IWM_RING0)
#include <string.h>
#endif
#include "iwm_init_fw.h"
#include "iwm_transport.h"

#define CSR_INT                    0x008UL
#define CSR_INT_MASK               0x00cUL
#define CSR_FH_INT_STATUS          0x010UL
#define CSR_RESET                  0x020UL
#define CSR_RESET_SW               0x00000080UL
#define CSR_RESET_MASTER_DISABLED  0x00000100UL
#define CSR_RESET_STOP_MASTER      0x00000200UL
#define HBUS_TARG_PRPH_WADDR       0x444UL
#define HBUS_TARG_PRPH_WDAT        0x44cUL
#define HBUS_PRPH_DWORD            (3UL << 24)
#define RFH_RXF_DMA_CFG            0x00a09820UL
#define RFH_RXF_RXQ_ACTIVE         0x00a0980cUL
#define MCC_SOURCE_GET_CURRENT     0x10U

static uint32_t rd(struct iwm_init_fw *st, uint32_t reg)
{
    return st->adapter->ops->mmio_read32(st->adapter->os_context, reg);
}

static void wr(struct iwm_init_fw *st, uint32_t reg, uint32_t v)
{
    st->adapter->ops->mmio_write32(st->adapter->os_context, reg, v);
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

/* RX notification hook: PHY-DB results are kept for the runtime image
 * (Linux iwl_phy_db_set_section), INIT_COMPLETE ends calibration. */
static void on_notif(void *ctx, uint8_t cmd, uint8_t group, uint16_t seq,
                     const uint8_t *payload, uint32_t len)
{
    struct iwm_init_fw *st = (struct iwm_init_fw *)ctx;
    uint16_t size;
    (void)seq;
    ++st->notif_count;
    st->last_notif = cmd;
    /* Linux iwl_notification_wait_notify(): a legacy command ID matches a
     * packet in group 0 or in LONG_GROUP (1), the DEF_ID() form. */
    if (group > 1U)
        goto other;
    if (cmd == IWM_CMD_INIT_COMPLETE_NOTIF) {
        st->init_complete = 1;
        return;
    }
    if (cmd != IWM_CMD_CALIB_RES_PHY_DB || len < IWM_PHYDB_HDR_SIZE ||
        !st->pool.vaddr)
        goto other;
    /* iwl_calib_res_notif_phy_db: le16 type, le16 length, data.  The
     * 9560 log and the Linux capture agree: payload 52 -> length 48,
     * 3084 -> 3080, 16 -> 12, 8 -> 4 (no reserved word). */
    size = iwm_get_le16(payload + 2);
    if ((uint32_t)size > len - IWM_PHYDB_HDR_SIZE ||
        st->phydb_used + 4U + size > IWM_PHYDB_POOL_SIZE)
        goto other;
    {
        uint8_t *dst = (uint8_t *)st->pool.vaddr + IWM_NVM_POOL_SIZE +
                       st->phydb_used;
        dst[0] = payload[0];
        dst[1] = payload[1];
        put_le16(dst + 2, size);
        memcpy(dst + 4, payload + IWM_PHYDB_HDR_SIZE, size);
    }
    st->phydb_used += 4U + size;
    ++st->phydb_count;
    return;
other:
    if (st->notif_other < IWM_INIT_OTHER_LOG)
        st->other_log[st->notif_other] =
            (uint32_t)cmd | ((uint32_t)group << 8) |
            ((len > 0xffffUL ? 0xffffUL : len) << 16);
    ++st->notif_other;
}

void iwm_init_fw_init(struct iwm_init_fw *st, struct iwm_adapter *a,
                      struct iwm_rx_queue *rxq, struct iwm_tx_transport *tx)
{
    if (!st)
        return;
    memset(st, 0, sizeof(*st));
    st->adapter = a;
    st->rxq = rxq;
    st->tx = tx;
}

void iwm_init_fw_free(struct iwm_init_fw *st)
{
    if (!st || !st->adapter || !st->adapter->ops)
        return;
    if (st->pool.vaddr && st->adapter->ops->dma_free)
        st->adapter->ops->dma_free(st->adapter->os_context, &st->pool);
    memset(&st->pool, 0, sizeof(st->pool));
}

/* Send one legacy command and wait for its response; the payload of the
 * response (first resp_cap bytes) lands in st->scratch. */
static int cmd_wait(struct iwm_init_fw *st, uint8_t cmd, const void *payload,
                    uint16_t len, uint32_t *resp_len)
{
    uint16_t seq = 0;
    int rc;
    st->rxq->resp_buf = st->scratch;
    st->rxq->resp_cap = (uint16_t)sizeof(st->scratch);
    rc = iwm_tx_send_cmd(st->tx, cmd, 0, payload, len, &seq);
    if (rc == IWM_OK)
        rc = iwm_rx_wait_resp(st->rxq, seq, IWM_INIT_CMD_TIMEOUT_US);
    st->rxq->resp_buf = NULL;
    st->rxq->resp_cap = 0;
    if (rc == IWM_OK) {
        iwm_tx_cmd_done(st->tx, seq);
        if (st->rxq->resp_cmd != cmd)
            rc = IWM_EIO;
    }
    if (rc != IWM_OK) {
        st->fail_cmd = cmd;
        st->fail_seq = seq;
        return rc;
    }
    if (resp_len)
        *resp_len = st->rxq->resp_len;
    return IWM_OK;
}

/* Linux iwl_nvm_read_section(): 2 KiB chunks until a short read; a non-zero
 * status on the first chunk means the section is absent. */
static int read_nvm(struct iwm_init_fw *st)
{
    uint8_t req[8];
    unsigned type;
    int rc;
    for (type = 0; type < IWM_NVM_NUM_SECTIONS; ++type) {
        struct iwm_nvm_section *s = &st->nvm[type];
        uint16_t off = 0;
        s->offset = st->nvm_used;
        for (;;) {
            uint32_t rlen = 0;
            uint16_t bytes, status;
            memset(req, 0, sizeof(req));
            req[0] = 0;                       /* op: read */
            req[1] = 0;                       /* target: cache */
            put_le16(req + 2, (uint16_t)type);
            put_le16(req + 4, off);
            put_le16(req + 6, IWM_NVM_CHUNK);
            rc = cmd_wait(st, IWM_CMD_NVM_ACCESS, req, sizeof(req), &rlen);
            if (rc != IWM_OK)
                return rc;
            if (rlen < 8U)
                return IWM_EFORMAT;
            bytes = iwm_get_le16(st->scratch + 2);
            status = iwm_get_le16(st->scratch + 6);
            s->status = status;
            if (status != 0)
                break;
            if ((uint32_t)bytes > rlen - 8U || bytes > IWM_NVM_CHUNK)
                return IWM_EFORMAT;
            if (st->nvm_used + bytes > IWM_NVM_POOL_SIZE)
                return IWM_EOVERFLOW;
            memcpy((uint8_t *)st->pool.vaddr + st->nvm_used,
                   st->scratch + 8, bytes);
            st->nvm_used += bytes;
            s->length = (uint16_t)(s->length + bytes);
            ++s->chunks;
            off = (uint16_t)(off + bytes);
            if (bytes < IWM_NVM_CHUNK)
                break;
        }
        s->present = s->length != 0;
    }
    return IWM_OK;
}

int iwm_init_fw_run(struct iwm_init_fw *st)
{
    const struct iwm_fw_image *im;
    uint8_t buf[28];
    uint32_t elapsed, rlen;
    int rc;
    if (!st || !st->adapter || !st->rxq || !st->tx || !st->adapter->ops ||
        !st->adapter->ops->dma_alloc || !st->adapter->ops->delay_us)
        return IWM_EINVAL;
    if (!st->tx->scheduler_started || !st->rxq->alive_seen)
        return IWM_ESTATE;
    im = &st->adapter->firmware.catalog;
    if (!st->pool.vaddr) {
        rc = st->adapter->ops->dma_alloc(st->adapter->os_context,
                                         IWM_NVM_POOL_SIZE + IWM_PHYDB_POOL_SIZE,
                                         16U, &st->pool);
        if (rc != IWM_OK)
            return st->rc = rc;
    }
    memset(st->nvm, 0, sizeof(st->nvm));
    st->nvm_used = 0;
    st->phydb_used = 0;
    st->phydb_count = 0;
    st->init_complete = 0;
    st->notif_count = 0;
    st->notif_other = 0;
    memset(st->other_log, 0, sizeof(st->other_log));
    st->mcc_rc = IWM_ENOTSUP;
    st->mcc_len = 0;
    st->rxq->notif_cb = on_notif;
    st->rxq->notif_ctx = st;

    st->stage = IWM_INIT_NVM;
    rc = read_nvm(st);
    if (rc != IWM_OK)
        goto out;

    /* Linux iwl_send_tx_ant_cfg(): FW_PHY_CFG_TX_CHAIN (bits 19:16). */
    st->stage = IWM_INIT_TX_ANT;
    st->tx_ant = (im->phy_config >> 16) & 0xfUL;
    put_le32(buf, st->tx_ant);
    rc = cmd_wait(st, IWM_CMD_TX_ANT_CONFIGURATION, buf, 4, NULL);
    if (rc != IWM_OK)
        goto out;

    /* Linux iwl_send_phy_cfg_cmd() with the INIT image's DEF_CALIB. */
    st->stage = IWM_INIT_PHY_CFG;
    st->phy_cfg = im->phy_config;
    if (st->adapter->cfg && (st->adapter->cfg->phy_flags & IWM_CFG_SHARED_CLK))
        st->phy_cfg |= IWM_FW_PHY_CFG_SHARED_CLK;
    st->flow_trigger = im->calib_flow[IWM_UCODE_TYPE_INIT];
    st->event_trigger = im->calib_event[IWM_UCODE_TYPE_INIT];
    put_le32(buf, st->phy_cfg);
    put_le32(buf + 4, st->flow_trigger);
    put_le32(buf + 8, st->event_trigger);
    rc = cmd_wait(st, IWM_CMD_PHY_CONFIGURATION, buf, 12, NULL);
    if (rc != IWM_OK)
        goto out;

    st->stage = IWM_INIT_CALIB_WAIT;
    for (elapsed = 0; !st->init_complete; elapsed += 100U) {
        if (elapsed >= IWM_INIT_CALIB_TIMEOUT_US) {
            rc = IWM_ETIMEDOUT;
            goto out;
        }
        rc = iwm_rx_drain(st->rxq);
        if (rc != IWM_OK && rc != IWM_EFORMAT && rc != IWM_EIO)
            goto out;
        if (!st->init_complete)
            st->adapter->ops->delay_us(st->adapter->os_context, 100U);
    }

    /* MCC query as sent by Linux on this card ("ZZ", GET_CURRENT); the
     * response carries the regulatory channel list.  Not fatal. */
    st->stage = IWM_INIT_MCC;
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x5a;
    buf[1] = 0x5a;
    buf[2] = MCC_SOURCE_GET_CURRENT;
    rlen = 0;
    st->mcc_rc = cmd_wait(st, IWM_CMD_MCC_UPDATE, buf, sizeof(buf), &rlen);
    if (st->mcc_rc == IWM_OK) {
        st->mcc_len = (uint16_t)rlen;
        memcpy(st->mcc_resp, st->scratch,
               IWM_MIN(rlen, (uint32_t)sizeof(st->mcc_resp)));
    }
    rc = IWM_OK;
    st->stage = IWM_INIT_DONE;
out:
    st->rc = rc;
    return rc;
}

int iwm_init_fw_stop(struct iwm_init_fw *st)
{
    uint32_t i;
    if (!st || !st->adapter || !st->adapter->ops)
        return IWM_EINVAL;
    wr(st, CSR_INT_MASK, 0);
    wr(st, CSR_INT, 0xffffffffUL);
    wr(st, CSR_FH_INT_STATUS, 0xffffffffUL);
    if (st->tx)
        iwm_tx_stop(st->tx);
    if (st->rxq) {
        if (iwm_grab_nic_access(st->adapter) == IWM_OK) {
            wr(st, HBUS_TARG_PRPH_WADDR, RFH_RXF_DMA_CFG | HBUS_PRPH_DWORD);
            wr(st, HBUS_TARG_PRPH_WDAT, 0);
            wr(st, HBUS_TARG_PRPH_WADDR, RFH_RXF_RXQ_ACTIVE | HBUS_PRPH_DWORD);
            wr(st, HBUS_TARG_PRPH_WDAT, 0);
            iwm_release_nic_access(st->adapter);
        }
        st->rxq->notif_cb = NULL;
        st->rxq->notif_ctx = NULL;
        st->rxq->programmed = 0;
        st->rxq->alive_seen = 0;
    }
    wr(st, CSR_RESET, rd(st, CSR_RESET) | CSR_RESET_STOP_MASTER);
    for (i = 0; i < 100U; ++i) {
        if (rd(st, CSR_RESET) & CSR_RESET_MASTER_DISABLED)
            break;
        st->adapter->ops->delay_us(st->adapter->os_context, 10U);
    }
    wr(st, CSR_RESET, rd(st, CSR_RESET) | CSR_RESET_SW);
    st->adapter->ops->delay_us(st->adapter->os_context, 6000U);
    /* Ready for iwm_fw_loader_start(IWM_FW_SEC_RUNTIME): the image and the
     * firmware session (LOADING) are kept. */
    st->adapter->state = IWM_ADAPTER_PROBED;
    st->stage = IWM_INIT_STOPPED;
    return IWM_OK;
}

const uint8_t *iwm_init_fw_nvm(const struct iwm_init_fw *st, unsigned type,
                               uint16_t *length)
{
    if (!st || type >= IWM_NVM_NUM_SECTIONS || !st->nvm[type].present ||
        !st->pool.vaddr)
        return NULL;
    if (length)
        *length = st->nvm[type].length;
    return (const uint8_t *)st->pool.vaddr + st->nvm[type].offset;
}
