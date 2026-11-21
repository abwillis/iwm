/* SPDX-License-Identifier: GPL-2.0-only */
/* Milestone E2: firmware side of station association, replaying the Linux
 * iwlwifi command sequence captured on this 9560 (core/iwm_sta_seq.c). */
#if !defined(IWM_RING0)
#include <string.h>
#endif
#include "iwm_sta.h"

static uint8_t g_sta_cmd[160];   /* largest entry: MAC_CONTEXT, 148 bytes */
static uint8_t g_sta_tx[IWM_TX_TXCMD_SIZE];

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

void iwm_sta_init(struct iwm_sta *sta, struct iwm_rt_fw *rt,
                  struct iwm_tx_transport *tx)
{
    if (!sta)
        return;
    memset(sta, 0, sizeof(*sta));
    sta->rt = rt;
    sta->tx = tx;
    sta->phase = IWM_STP_IDLE;
}

uint8_t iwm_sta_channel_24(uint32_t khz)
{
    if (khz == 2484000UL)
        return 14;
    if (khz < 2412000UL || khz > 2472000UL || (khz - 2407000UL) % 5000UL)
        return 0;
    return (uint8_t)((khz - 2407000UL) / 5000UL);
}

int iwm_sta_build(const struct iwm_sta *sta, const struct iwm_sta_seq_cmd *c,
                  uint8_t *out)
{
    if (!sta || !c || !out || c->len > sizeof(g_sta_cmd))
        return IWM_EINVAL;
    memset(out, 0, c->len ? c->len : 1U);
    if (c->data && c->len)
        memcpy(out, c->data, c->len);
    if (c->mac_off != IWM_SS_NONE) {
        if ((uint32_t)c->mac_off + 6U > c->len)
            return IWM_EFORMAT;
        memcpy(out + c->mac_off, sta->mac, 6);
    }
    if (c->bssid_off != IWM_SS_NONE) {
        if ((uint32_t)c->bssid_off + 6U > c->len)
            return IWM_EFORMAT;
        memcpy(out + c->bssid_off, sta->bssid, 6);
    }
    if (c->flags & IWM_SS_PHY_CHAN) {
        if (c->len < IWM_STA_PHY_BAND_OFF + 1U)
            return IWM_EFORMAT;
        put_le32(out + IWM_STA_PHY_CHAN_OFF, sta->channel);
        out[IWM_STA_PHY_BAND_OFF] = IWM_STA_PHY_BAND_24;
    }
    if (c->flags & IWM_SS_MACCTX) {
        if (c->len < IWM_STA_MAC_AID_OFF + 4U)
            return IWM_EFORMAT;
        /* Only words the captured command filled in are replaced. */
        if (iwm_get_le32(out + IWM_STA_MAC_BI_OFF))
            put_le32(out + IWM_STA_MAC_BI_OFF, sta->beacon_int);
        if (iwm_get_le32(out + IWM_STA_MAC_DTIM_OFF))   /* DTIM period 1 */
            put_le32(out + IWM_STA_MAC_DTIM_OFF, sta->beacon_int);
        if (iwm_get_le32(out + IWM_STA_MAC_AID_OFF))
            put_le32(out + IWM_STA_MAC_AID_OFF, sta->aid);
        if (sta->filter_or && c->len >= IWM_STA_MAC_FILTER_OFF + 4U)
            put_le32(out + IWM_STA_MAC_FILTER_OFF,
                     iwm_get_le32(out + IWM_STA_MAC_FILTER_OFF) | sta->filter_or);
    }
    if (c->flags & IWM_SS_TE_REMOVE) {
        if (c->len < IWM_STA_TE_ID_OFF + 4U)
            return IWM_EFORMAT;
        put_le32(out + IWM_STA_TE_ID_OFF, sta->te_id);
    }
    return IWM_OK;
}

static void log_cmd(struct iwm_sta *sta, uint16_t id, int rc, uint16_t rlen,
                    const uint8_t *resp)
{
    struct iwm_sta_log *l;
    if (sta->log_n >= IWM_STA_LOG)
        return;
    l = &sta->log[sta->log_n++];
    l->id = id;
    l->rc = (int32_t)rc;
    l->resp_len = rlen;
    l->resp0 = iwm_get_le32(resp);
    l->resp1 = iwm_get_le32(resp + 4);
    l->resp2 = iwm_get_le32(resp + 8);
}

static int send_one(struct iwm_sta *sta, const struct iwm_sta_seq_cmd *c)
{
    static uint8_t resp[16];
    uint16_t rlen = 0;
    int rc;
    memset(resp, 0, sizeof(resp));
    rc = iwm_sta_build(sta, c, g_sta_cmd);
    if (rc == IWM_OK && (c->flags & IWM_SS_SET_PTR)) {
        unsigned q = g_sta_cmd[IWM_STA_SCD_QUEUE_OFF];
        rc = iwm_tx_frame_queue_reset(sta->tx, q);
        if (rc == IWM_OK)
            rc = iwm_tx_queue_set_ptr(sta->tx, q, 0);
    }
    if (rc == IWM_OK)
        rc = iwm_rt_fw_send(sta->rt, c->id, g_sta_cmd, c->len, resp,
                            sizeof(resp), &rlen);
    log_cmd(sta, c->id, rc, rlen, resp);
    if (rc != IWM_OK)
        return rc;
    if ((c->flags & IWM_SS_TE_ADD) && rlen >= 12U) {
        sta->te_id = iwm_get_le32(resp + IWM_STA_TE_ID_OFF);
        sta->te_active = 1;
    }
    if (c->flags & IWM_SS_TE_REMOVE)
        sta->te_active = 0;
    return IWM_OK;
}

/* d20 log: the first disconnect sent TRIGGER_RX_QUEUES_NOTIF (0x502, Linux
 * iwl_mvm_sync_rx_queues_internal) as command 45 of 59; it and all 14 commands
 * after it timed out (15 x 500 ms = the 7501 ms IOCTL) and the firmware never
 * answered anything again.  The command only synchronises several host RX
 * queues and block-ack reorder buffers; this driver has one queue and no
 * block-ack sessions, so it is not sent (IWM_SS_SKIP).  A timeout also ends
 * the set at once, whatever the entry's flags: the command queue is stalled
 * and every further command would just wait out its own 500 ms. */
static int run_set(struct iwm_sta *sta, const struct iwm_sta_seq_cmd *t,
                   unsigned n)
{
    unsigned i;
    int rc;
    for (i = 0; i < n; ++i) {
        if (t[i].flags & IWM_SS_SKIP)
            continue;
        rc = send_one(sta, &t[i]);
        if (rc == IWM_OK)
            continue;
        if (rc == IWM_ETIMEDOUT || sta->rt->dead ||
            !(t[i].flags & IWM_SS_NONFATAL)) {
            sta->rc = rc;
            sta->fail_id = t[i].id;
            return rc;
        }
    }
    return IWM_OK;
}

void iwm_sta_notif(void *ctx, uint8_t cmd, uint8_t group, uint16_t seq,
                   const uint8_t *payload, uint32_t len)
{
    struct iwm_sta *sta = (struct iwm_sta *)ctx;
    unsigned q;
    if (!sta)
        return;
    if (group <= 1U && cmd == IWM_STA_CMD_TIME_EVENT_NOTIF) {
        sta->te_started = 1;
        sta->te_notif_len = (uint16_t)IWM_MIN(len, 0xffffUL);
        memcpy(sta->te_notif, payload,
               IWM_MIN(len, (uint32_t)sizeof(sta->te_notif)));
        return;
    }
    if (group != 0 || cmd != IWM_TX_CMD_TX)
        return;
    seq &= 0x7fffU;
    q = (seq >> 8) & 0x1fU;
    if (q != IWM_TX_DATA_QUEUE && q != IWM_TX_MGMT_QUEUE)
        return;
    iwm_tx_frame_done(sta->tx, seq);
    if (sta->txresp_n < IWM_STA_TXRESP_LOG) {
        sta->txresp_seq[sta->txresp_n] = seq;
        sta->txresp_len[sta->txresp_n] = (uint16_t)IWM_MIN(len, 0xffffUL);
        memcpy(sta->txresp[sta->txresp_n], payload,
               IWM_MIN(len, (uint32_t)IWM_STA_TXRESP_BYTES));
        ++sta->txresp_n;
    }
    ++sta->tx_resp;
}

int iwm_sta_prepare(struct iwm_sta *sta, const struct iwm_scan_result *bss,
                    const uint8_t mac[6])
{
    const struct iwm_os_ops *ops;
    uint32_t us;
    int rc;
    if (!sta || !sta->rt || !sta->tx || !bss || !mac || !sta->rt->adapter)
        return IWM_EINVAL;
    ops = sta->rt->adapter->ops;
    if (!ops || !ops->delay_us)
        return IWM_EINVAL;
    if (sta->phase != IWM_STP_IDLE)
        return IWM_ESTATE;
    sta->channel = iwm_sta_channel_24(bss->ds_config_khz);
    if (!sta->channel)
        return sta->rc = IWM_ENOTSUP;   /* only 2.4 GHz was captured */
    memcpy(sta->mac, mac, 6);
    memcpy(sta->bssid, bss->bssid, 6);
    sta->beacon_int = (uint16_t)(bss->beacon_period ? bss->beacon_period : 100U);
    sta->aid = 0;
    sta->te_id = 0;
    sta->te_active = 0;
    sta->te_started = 0;
    sta->te_wait_us = 0;
    sta->te_notif_len = 0;
    sta->rc = IWM_OK;
    sta->fail_id = 0;
    sta->log_n = 0;
    sta->tx_sent = 0;
    sta->tx_resp = 0;
    sta->txresp_n = 0;
    /* From here on a teardown is needed even if a command fails. */
    sta->phase = IWM_STP_PREPARED;
    rc = run_set(sta, iwm_sta_seq_pre, IWM_STA_PRE);
    if (rc != IWM_OK) {
        sta->phase = IWM_STP_FAILED;
        return rc;
    }
    /* Linux protects the session and waits for the time event to start
     * before the first frame; here the wait is bounded and only logged. */
    for (us = 0; !sta->te_started && us < IWM_STA_TE_WAIT_US; us += 100U) {
        (void)iwm_rx_drain(sta->rt->rxq);
        if (!sta->te_started)
            ops->delay_us(sta->rt->adapter->os_context, 100U);
    }
    sta->te_wait_us = us;
    return IWM_OK;
}

int iwm_sta_tx_mgmt(struct iwm_sta *sta, const uint8_t *frame, uint16_t len)
{
    uint16_t seq = 0;
    int rc;
    if (!sta || !frame || len < 24U)
        return IWM_EINVAL;
    if (sta->phase != IWM_STP_PREPARED && sta->phase != IWM_STP_AUTHED &&
        sta->phase != IWM_STP_UP)
        return IWM_ESTATE;
    /* Association request: Linux sends 73-74 between the authentication
     * response and the request. */
    if ((frame[0] == 0x00 || frame[0] == 0x20) &&
        sta->phase == IWM_STP_PREPARED) {
        rc = run_set(sta, iwm_sta_seq_auth, IWM_STA_AUTHED);
        if (rc != IWM_OK) {
            sta->phase = IWM_STP_FAILED;
            return rc;
        }
        sta->phase = IWM_STP_AUTHED;
    }
    memcpy(g_sta_tx, iwm_sta_mgmt_txcmd, IWM_TX_TXCMD_SIZE);
    /* Linux pm_frame_timeout: 3 for (re)association requests, 0 for action
     * frames (ADDBA response in the d26 capture), 2 for the rest. */
    put_le16(g_sta_tx + IWM_STA_TX_PM_TIMEOUT_OFF,
             (uint16_t)((frame[0] == 0x00 || frame[0] == 0x20) ? 3U :
                        (frame[0] == 0xd0 ? 0U : 2U)));
    rc = iwm_tx_send_frame(sta->tx, IWM_TX_MGMT_QUEUE, g_sta_tx, frame, len,
                           24U, &seq);
    if (rc == IWM_OK)
        ++sta->tx_sent;
    return rc;
}

int iwm_sta_up(struct iwm_sta *sta, uint16_t aid)
{
    int rc;
    if (!sta)
        return IWM_EINVAL;
    if (sta->phase != IWM_STP_AUTHED && sta->phase != IWM_STP_PREPARED)
        return IWM_ESTATE;
    sta->aid = aid;
    rc = run_set(sta, iwm_sta_seq_up, IWM_STA_UP);
    if (rc != IWM_OK) {
        sta->phase = IWM_STP_FAILED;
        return rc;
    }
    sta->phase = IWM_STP_UP;
    if (sta->te_active)
        (void)run_set(sta, iwm_sta_seq_te, IWM_STA_TE_DONE);
    return IWM_OK;
}

void iwm_sta_teardown(struct iwm_sta *sta)
{
    if (!sta || sta->phase == IWM_STP_IDLE)
        return;
    if (sta->phase == IWM_STP_UP) {
        (void)run_set(sta, iwm_sta_seq_down, IWM_STA_DOWN);
    } else {
        if (sta->te_active)
            (void)run_set(sta, iwm_sta_seq_te, IWM_STA_TE_DONE);
        (void)run_set(sta, iwm_sta_seq_fail, IWM_STA_ABORT);
    }
    (void)iwm_tx_frame_queue_reset(sta->tx, IWM_TX_DATA_QUEUE);
    (void)iwm_tx_frame_queue_reset(sta->tx, IWM_TX_MGMT_QUEUE);
    sta->te_active = 0;
    sta->aid = 0;
    sta->phase = IWM_STP_IDLE;
}
