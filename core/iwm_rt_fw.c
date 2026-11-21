/* SPDX-License-Identifier: GPL-2.0-only */
/* Runtime firmware bring-up after runtime ALIVE (milestone C). */
#if !defined(IWM_RING0)
#include <string.h>
#endif
#include "iwm_rt_fw.h"
#include "iwm_transport.h"

#define RT_NO_SECTION 0xffffU
#define RT_COPY_CHUNK 16384U

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

/* One buffer shared by every command payload; it lives in the (static)
 * state, not on the ring-0 stack. */
static uint8_t g_rt_cmd[IWM_TX_CMD_MAX_PAYLOAD];

static void hist_note(struct iwm_rt_fw *rt, uint8_t cmd, uint8_t group,
                      uint32_t len)
{
    uint16_t key = (uint16_t)(((uint16_t)group << 8) | cmd);
    uint16_t i;
    for (i = 0; i < rt->hist_n; ++i)
        if (rt->hist[i].key == key)
            break;
    if (i == rt->hist_n) {
        if (rt->hist_n >= IWM_RT_HIST) {
            ++rt->hist_lost;
            return;
        }
        rt->hist[i].key = key;
        rt->hist[i].max_len = 0;
        rt->hist[i].count = 0;
        ++rt->hist_n;
    }
    ++rt->hist[i].count;
    if (len > rt->hist[i].max_len)
        rt->hist[i].max_len = (uint16_t)IWM_MIN(len, 0xffffUL);
}

static void on_notif(void *ctx, uint8_t cmd, uint8_t group, uint16_t seq,
                     const uint8_t *payload, uint32_t len)
{
    struct iwm_rt_fw *rt = (struct iwm_rt_fw *)ctx;
    if (rt->notif_hook)
        rt->notif_hook(rt->notif_hook_ctx, cmd, group, seq, payload, len);
    hist_note(rt, cmd, group, len);
    /* Linux matches a legacy notification ID in group 0 or LONG_GROUP. */
    if (group <= 1U && cmd == IWM_CMD_SCAN_COMPLETE_UMAC && !rt->scan_done) {
        rt->scan_done = 1;
        rt->scan_complete_len = (uint16_t)IWM_MIN(len, 0xffffUL);
        memcpy(rt->scan_complete, payload,
               IWM_MIN(len, (uint32_t)sizeof(rt->scan_complete)));
    }
    if (group <= 1U && cmd == IWM_CMD_RX_MPDU &&
        (rt->stage == IWM_RT_POST_SEQ || rt->stage == IWM_RT_SCAN_WAIT)) {
        int prc;
        ++rt->scan_frames;
        prc = iwm_scan_parse_mpdu(payload, len, &rt->scan_tmp);
        if (prc == IWM_OK) {
            if (iwm_genmac_scan_upsert(rt->adapter, &rt->scan_tmp) == IWM_OK)
                ++rt->scan_parsed;
            else
                ++rt->scan_lost;
        } else if (prc == IWM_ENOTSUP) {
            ++rt->scan_skipped;
        } else {
            ++rt->scan_bad;
        }
    }
    if (group <= 1U && cmd == IWM_CMD_RX_MPDU && rt->mgmt_cb) {
        const uint8_t *frame;
        uint32_t frame_len;
        if (iwm_rx_mpdu_frame(payload, len, &frame, &frame_len) == IWM_OK) {
            ++rt->mgmt_frames;
            rt->mgmt_cb(rt->mgmt_ctx, frame, frame_len);
        }
    }
    if (group <= 1U && cmd == IWM_CMD_RX_MPDU &&
        rt->mpdu_n < IWM_RT_MPDU_SAMPLES) {
        rt->mpdu_len[rt->mpdu_n] = (uint16_t)IWM_MIN(len, 0xffffUL);
        memcpy(rt->mpdu[rt->mpdu_n], payload,
               IWM_MIN(len, (uint32_t)IWM_RT_MPDU_SAMPLE_LEN));
        ++rt->mpdu_n;
    }
    if (rt->notif_count < IWM_RT_NOTIF_LOG)
        rt->notif_log[rt->notif_count] =
            (uint32_t)cmd | ((uint32_t)group << 8) |
            ((len > 0xffffUL ? 0xffffUL : len) << 16);
    ++rt->notif_count;
}

void iwm_rt_fw_init(struct iwm_rt_fw *rt, struct iwm_adapter *a,
                    struct iwm_rx_queue *rxq, struct iwm_tx_transport *tx,
                    const struct iwm_init_fw *init)
{
    if (!rt)
        return;
    memset(rt, 0, sizeof(*rt));
    rt->adapter = a;
    rt->rxq = rxq;
    rt->tx = tx;
    rt->init = init;
    rt->paging_sep = RT_NO_SECTION;
    rt->css_sec = RT_NO_SECTION;
    rt->data_sec = RT_NO_SECTION;
}

void iwm_rt_fw_set_mgmt_cb(struct iwm_rt_fw *rt, iwm_rt_mgmt_fn cb, void *ctx)
{
    if (!rt)
        return;
    rt->mgmt_cb = cb;
    rt->mgmt_ctx = ctx;
}

void iwm_rt_fw_set_notif_hook(struct iwm_rt_fw *rt, iwm_rt_notif_fn fn,
                              void *ctx)
{
    if (!rt)
        return;
    rt->notif_hook = fn;
    rt->notif_hook_ctx = ctx;
    /* The hook is called from the runtime notification handler. */
    if (rt->rxq) {
        rt->rxq->notif_cb = on_notif;
        rt->rxq->notif_ctx = rt;
    }
}

void iwm_rt_fw_free(struct iwm_rt_fw *rt)
{
    if (!rt || !rt->adapter || !rt->adapter->ops ||
        !rt->adapter->ops->dma_free)
        return;
    if (rt->rxq && rt->rxq->notif_ctx == rt) {
        rt->rxq->notif_cb = NULL;
        rt->rxq->notif_ctx = NULL;
    }
    if (rt->pages.vaddr)
        rt->adapter->ops->dma_free(rt->adapter->os_context, &rt->pages);
    if (rt->css.vaddr)
        rt->adapter->ops->dma_free(rt->adapter->os_context, &rt->css);
    memset(&rt->pages, 0, sizeof(rt->pages));
    memset(&rt->css, 0, sizeof(rt->css));
}

static int copy_section(struct iwm_rt_fw *rt, const struct iwm_fw_section *sec,
                        uint8_t *dst, uint32_t length)
{
    uint32_t off, n;
    int rc;
    for (off = 0; off < length; off += n) {
        n = IWM_MIN(length - off, (uint32_t)RT_COPY_CHUNK);
        rc = iwm_blob_read(rt->adapter->firmware.store,
                           sec->data_offset + off, dst + off, (uint16_t)n);
        if (rc != IWM_OK)
            return rc;
    }
    return IWM_OK;
}

/* Linux iwl_alloc_fw_paging_mem() + iwl_fill_paging_mem(). */
int iwm_rt_fw_prepare_paging(struct iwm_rt_fw *rt)
{
    const struct iwm_fw_image *im;
    uint16_t i;
    uint32_t pages;
    int rc;
    if (!rt || !rt->adapter || !rt->adapter->ops ||
        !rt->adapter->ops->dma_alloc || !rt->adapter->firmware.store)
        return IWM_EINVAL;
    rt->stage = IWM_RT_PAGING_PREP;
    im = &rt->adapter->firmware.catalog;
    rt->paging_sep = RT_NO_SECTION;
    rt->css_sec = RT_NO_SECTION;
    rt->data_sec = RT_NO_SECTION;
    for (i = 0; i < im->section_count; ++i) {
        const struct iwm_fw_section *s = &im->section[i];
        if (!(s->flags & IWM_FW_SEC_RUNTIME))
            continue;
        if (rt->paging_sep == RT_NO_SECTION) {
            if ((s->flags & IWM_FW_SEC_SEPARATOR) &&
                s->dst_addr == IWM_UCODE_SECTION_PAGING)
                rt->paging_sep = i;
            continue;
        }
        if (s->flags & IWM_FW_SEC_SEPARATOR)
            break;
        if (rt->css_sec == RT_NO_SECTION)
            rt->css_sec = i;
        else {
            rt->data_sec = i;
            break;
        }
    }
    rt->paging_blocks = 0;
    rt->pages_last = 0;
    rt->paging_size = 0;
    if (rt->paging_sep == RT_NO_SECTION || im->paging_mem_size == 0)
        return rt->rc = IWM_OK;          /* Linux: no paging */
    if (rt->css_sec == RT_NO_SECTION || rt->data_sec == RT_NO_SECTION)
        return rt->rc = IWM_EFORMAT;     /* Linux: "Paging: Missing CSS" */
    if (im->section[rt->css_sec].data_length > IWM_FW_PAGING_SIZE ||
        im->section[rt->data_sec].data_length < im->paging_mem_size)
        return rt->rc = IWM_EFORMAT;
    pages = im->paging_mem_size / IWM_FW_PAGING_SIZE;
    rt->paging_blocks = (pages + IWM_PAGING_PAGES_PER_BLOCK - 1U) /
                        IWM_PAGING_PAGES_PER_BLOCK;
    rt->pages_last = pages - IWM_PAGING_PAGES_PER_BLOCK *
                     (rt->paging_blocks - 1U);
    if (rt->paging_blocks + 1U > IWM_PAGING_MAX_BLOCKS)
        return rt->rc = IWM_EOVERFLOW;
    rt->paging_size = im->paging_mem_size;
    if (!rt->css.vaddr) {
        rc = rt->adapter->ops->dma_alloc(rt->adapter->os_context,
                                         IWM_FW_PAGING_SIZE,
                                         IWM_FW_PAGING_SIZE, &rt->css);
        if (rc != IWM_OK)
            return rt->rc = rc;
    }
    if (!rt->pages.vaddr) {
        rc = rt->adapter->ops->dma_alloc(rt->adapter->os_context,
                                         rt->paging_blocks *
                                         IWM_PAGING_BLOCK_SIZE,
                                         IWM_PAGING_BLOCK_SIZE, &rt->pages);
        if (rc != IWM_OK)
            return rt->rc = rc;
    }
    if (iwm_dma32_check(rt->css.paddr, rt->css.size, IWM_FW_PAGING_SIZE) !=
            IWM_OK ||
        iwm_dma32_check(rt->pages.paddr, rt->pages.size,
                        IWM_FW_PAGING_SIZE) != IWM_OK)
        return rt->rc = IWM_ERANGE;
    memset(rt->css.vaddr, 0, rt->css.size);
    memset(rt->pages.vaddr, 0, rt->pages.size);
    rc = copy_section(rt, &im->section[rt->css_sec], (uint8_t *)rt->css.vaddr,
                      im->section[rt->css_sec].data_length);
    if (rc == IWM_OK)
        rc = copy_section(rt, &im->section[rt->data_sec],
                          (uint8_t *)rt->pages.vaddr, rt->paging_size);
    return rt->rc = rc;
}

/* Linux iwl_send_paging_cmd(): flags, block size as a power of two, block
 * count (CSS excluded), then one page-frame number per block, CSS first. */
void iwm_rt_fw_paging_cmd(const struct iwm_rt_fw *rt, uint8_t *out)
{
    uint32_t i;
    memset(out, 0, IWM_PAGING_CMD_SIZE);
    put_le32(out, IWM_PAGING_CMD_SECURED | IWM_PAGING_CMD_ENABLED |
                  rt->pages_last);
    put_le32(out + 4, IWM_PAGING_BLOCK_SHIFT);
    put_le32(out + 8, rt->paging_blocks);
    put_le32(out + 12, rt->css.paddr >> IWM_PAGING_PAGE_SHIFT);
    for (i = 0; i < rt->paging_blocks; ++i)
        put_le32(out + 16U + 4U * i,
                 (rt->pages.paddr + i * IWM_PAGING_BLOCK_SIZE) >>
                 IWM_PAGING_PAGE_SHIFT);
}

static int cmd_wait(struct iwm_rt_fw *rt, uint8_t cmd, uint8_t group,
                    const void *payload, uint16_t len)
{
    uint16_t seq = 0;
    int rc;
    if (rt->dead) {
        rt->fail_cmd = cmd;
        return IWM_EIO;
    }
    rt->rxq->resp_buf = rt->scratch;
    rt->rxq->resp_cap = (uint16_t)sizeof(rt->scratch);
    rc = iwm_tx_send_cmd(rt->tx, cmd, group, payload, len, &seq);
    if (rc == IWM_OK)
        rc = iwm_rx_wait_resp(rt->rxq, seq, IWM_RT_CMD_TIMEOUT_US);
    rt->rxq->resp_buf = NULL;
    rt->rxq->resp_cap = 0;
    if (rc == IWM_OK) {
        iwm_tx_cmd_done(rt->tx, seq);
        if (rt->rxq->resp_cmd != cmd)
            rc = IWM_EIO;
    }
    if (rc == IWM_OK) {
        rt->consec_timeouts = 0;
    } else {
        rt->fail_cmd = cmd;
        rt->fail_seq = seq;
        if (rc == IWM_ETIMEDOUT) {
            ++rt->cmd_timeouts;
            if (++rt->consec_timeouts >= IWM_RT_DEAD_AFTER)
                rt->dead = 1;
        }
    }
    return rc;
}

/* Send one replayed command: payload from the table (zero-filled past the
 * stored bytes), adapter MAC patched in, result appended to cmd_log. */
static int send_seq(struct iwm_rt_fw *rt, const struct iwm_rt_seq_cmd *c)
{
    struct iwm_rt_cmd_rec *r = NULL;
    int rc;
    if (c->len > IWM_TX_CMD_MAX_PAYLOAD)
        return IWM_EOVERFLOW;
    memset(g_rt_cmd, 0, c->len ? c->len : 1U);
    if (c->data && c->len)
        memcpy(g_rt_cmd, c->data, c->len);
    if (c->mac_off != IWM_RT_SEQ_NO_MAC) {
        if ((uint32_t)c->mac_off + 6U > c->len || !rt->adapter->mac_valid)
            return IWM_ESTATE;
        memcpy(g_rt_cmd + c->mac_off, rt->adapter->mac, 6);
    }
    if (c->flags & IWM_RT_SEQ_AUX_PTR) {
        rc = iwm_tx_queue_set_ptr(rt->tx, IWM_TX_AUX_QUEUE, 0);
        if (rc != IWM_OK)
            return rc;
    }
    if (rt->cmd_count < IWM_RT_CMD_LOG) {
        r = &rt->cmd_log[rt->cmd_count];
        memset(r, 0, sizeof(*r));
        r->id = c->id;
        r->len = c->len;
    }
    ++rt->cmd_count;
    memset(rt->scratch, 0, 8);
    rc = cmd_wait(rt, (uint8_t)c->id, (uint8_t)(c->id >> 8), g_rt_cmd, c->len);
    if (r) {
        r->rc = (int32_t)rc;
        if (rc == IWM_OK) {
            r->resp_len = rt->rxq->resp_len;
            r->resp0 = iwm_get_le32(rt->scratch);
            r->resp1 = iwm_get_le32(rt->scratch + 4);
        }
    }
    return rc;
}

int iwm_rt_fw_send(struct iwm_rt_fw *rt, uint16_t id, const uint8_t *payload,
                   uint16_t length, uint8_t *resp, uint16_t resp_cap,
                   uint16_t *resp_len)
{
    int rc;
    if (!rt || !rt->rxq || !rt->tx || (length && !payload))
        return IWM_EINVAL;
    if (!rt->tx->scheduler_started || !rt->rxq->alive_seen)
        return IWM_ESTATE;
    if (length > IWM_TX_CMD_MAX_PAYLOAD)
        return IWM_EOVERFLOW;
    if (length)
        memcpy(g_rt_cmd, payload, length);
    memset(rt->scratch, 0, sizeof(rt->scratch));
    rc = cmd_wait(rt, (uint8_t)id, (uint8_t)(id >> 8), g_rt_cmd, length);
    if (resp_len)
        *resp_len = rc == IWM_OK ? rt->rxq->resp_len : 0;
    if (rc == IWM_OK && resp && resp_cap)
        memcpy(resp, rt->scratch,
               IWM_MIN((uint32_t)resp_cap, (uint32_t)sizeof(rt->scratch)));
    return rc;
}

/* PHY-DB pool entry: le16 type, le16 size, data (iwm_init_fw on_notif). */
static const uint8_t *phydb_next(const struct iwm_rt_fw *rt, uint32_t *off)
{
    const uint8_t *base;
    const uint8_t *e;
    if (!rt->init || !rt->init->pool.vaddr || *off >= rt->init->phydb_used)
        return NULL;
    base = (const uint8_t *)rt->init->pool.vaddr + IWM_NVM_POOL_SIZE;
    e = base + *off;
    *off += 4U + iwm_get_le16(e + 2);
    return e;
}

/* Linux keeps one entry per (type, channel group); a later notification for
 * the same group replaces the earlier one. */
static int superseded(const struct iwm_rt_fw *rt, const uint8_t *e,
                      uint32_t after)
{
    uint16_t type = iwm_get_le16(e);
    uint16_t size = iwm_get_le16(e + 2);
    uint32_t off = after;
    const uint8_t *n;
    if ((type != IWM_PHY_DB_CALIB_CHG_PAPD &&
         type != IWM_PHY_DB_CALIB_CHG_TXP) || size < 2U)
        return 0;
    while ((n = phydb_next(rt, &off)) != NULL)
        if (iwm_get_le16(n) == type && iwm_get_le16(n + 2) >= 2U &&
            iwm_get_le16(n + 4) == iwm_get_le16(e + 4))
            return 1;
    return 0;
}

static int send_phydb_type(struct iwm_rt_fw *rt, uint16_t type, int required)
{
    uint32_t off = 0, after;
    const uint8_t *e;
    unsigned found = 0;
    int rc;
    while ((e = phydb_next(rt, &off)) != NULL) {
        uint16_t size;
        if (iwm_get_le16(e) != type)
            continue;
        after = off;
        if (superseded(rt, e, after)) {
            ++rt->phydb_skipped;
            continue;
        }
        size = iwm_get_le16(e + 2);
        if (4U + (uint32_t)size > IWM_TX_CMD_MAX_PAYLOAD) {
            rt->fail_cmd = IWM_CMD_PHY_DB;
            return IWM_EOVERFLOW;
        }
        /* Linux struct iwl_phy_db_cmd: le16 type, le16 length, data. */
        put_le16(g_rt_cmd, type);
        put_le16(g_rt_cmd + 2, size);
        memcpy(g_rt_cmd + 4, e + 4, size);
        rc = cmd_wait(rt, IWM_CMD_PHY_DB, 0, g_rt_cmd, (uint16_t)(4U + size));
        if (rc != IWM_OK)
            return rc;
        ++rt->phydb_sent;
        rt->phydb_bytes += size;
        ++found;
    }
    if (required && !found) {
        rt->fail_cmd = IWM_CMD_PHY_DB;
        return IWM_EFORMAT;   /* Linux: "Cannot get Phy DB ... section" */
    }
    return IWM_OK;
}

int iwm_rt_fw_run(struct iwm_rt_fw *rt)
{
    const struct iwm_fw_image *im;
    int rc;
    if (!rt || !rt->adapter || !rt->rxq || !rt->tx || !rt->adapter->ops)
        return IWM_EINVAL;
    if (!rt->tx->scheduler_started || !rt->rxq->alive_seen)
        return rt->rc = IWM_ESTATE;
    im = &rt->adapter->firmware.catalog;
    rt->notif_count = 0;
    memset(rt->notif_log, 0, sizeof(rt->notif_log));
    rt->phydb_sent = 0;
    rt->phydb_skipped = 0;
    rt->phydb_bytes = 0;
    rt->cmd_count = 0;
    rt->hist_n = 0;
    rt->hist_lost = 0;
    rt->rxq->notif_cb = on_notif;
    rt->rxq->notif_ctx = rt;

    if (rt->paging_blocks) {
        rt->stage = IWM_RT_PAGING_CMD;
        iwm_rt_fw_paging_cmd(rt, g_rt_cmd);
        rc = cmd_wait(rt, IWM_CMD_FW_PAGING_BLOCK, IWM_GROUP_LONG, g_rt_cmd,
                      IWM_PAGING_CMD_SIZE);
        if (rc != IWM_OK)
            goto out;
    }

    /* Linux iwl_mvm_up() after runtime ALIVE: debug config, shared-memory
     * query, smart-FIFO config, before the TX antenna / PHY-DB / PHY steps. */
    {
        unsigned i;
        for (i = 0; i < IWM_RT_PRE_CMDS; ++i) {
            rc = send_seq(rt, &iwm_rt_pre_cmds[i]);
            if (rc != IWM_OK &&
                !(iwm_rt_pre_cmds[i].flags & IWM_RT_SEQ_NONFATAL))
                goto out;
        }
    }

    rt->stage = IWM_RT_TX_ANT;
    put_le32(g_rt_cmd, (im->phy_config >> 16) & 0xfUL);
    rc = cmd_wait(rt, IWM_CMD_TX_ANT_CONFIGURATION, 0, g_rt_cmd, 4);
    if (rc != IWM_OK)
        goto out;

    /* Linux iwl_send_phy_db_data(): CFG, NCH, then every PAPD and TXP
     * channel group. */
    rt->stage = IWM_RT_PHY_DB;
    rc = send_phydb_type(rt, IWM_PHY_DB_CFG, 1);
    if (rc == IWM_OK)
        rc = send_phydb_type(rt, IWM_PHY_DB_CALIB_NCH, 1);
    if (rc == IWM_OK)
        rc = send_phydb_type(rt, IWM_PHY_DB_CALIB_CHG_PAPD, 0);
    if (rc == IWM_OK)
        rc = send_phydb_type(rt, IWM_PHY_DB_CALIB_CHG_TXP, 0);
    if (rc != IWM_OK)
        goto out;

    rt->stage = IWM_RT_PHY_CFG;
    rt->phy_cfg = im->phy_config;
    if (rt->adapter->cfg && (rt->adapter->cfg->phy_flags & IWM_CFG_SHARED_CLK))
        rt->phy_cfg |= IWM_FW_PHY_CFG_SHARED_CLK;
    rt->flow_trigger = im->calib_flow[IWM_UCODE_TYPE_REGULAR];
    rt->event_trigger = im->calib_event[IWM_UCODE_TYPE_REGULAR];
    put_le32(g_rt_cmd, rt->phy_cfg);
    put_le32(g_rt_cmd + 4, rt->flow_trigger);
    put_le32(g_rt_cmd + 8, rt->event_trigger);
    rc = cmd_wait(rt, IWM_CMD_PHY_CONFIGURATION, 0, g_rt_cmd, 12);
    if (rc != IWM_OK)
        goto out;
    rt->stage = IWM_RT_DONE;
out:
    rt->rc = rc;
    return rc;
}

static void scan_reset(struct iwm_rt_fw *rt)
{
    rt->hist_n = 0;
    rt->hist_lost = 0;
    rt->scan_done = 0;
    rt->scan_complete_len = 0;
    memset(rt->scan_complete, 0, sizeof(rt->scan_complete));
    rt->scan_wait_us = 0;
    rt->mpdu_n = 0;
    rt->scan_frames = 0;
    rt->scan_parsed = 0;
    rt->scan_skipped = 0;
    rt->scan_bad = 0;
    rt->scan_lost = 0;
    rt->rxq->notif_cb = on_notif;
    rt->rxq->notif_ctx = rt;
}

/* Drain RX until SCAN_COMPLETE_UMAC; RX MPDUs are parsed by on_notif. */
static int scan_wait(struct iwm_rt_fw *rt)
{
    uint32_t elapsed;
    int rc;
    rt->stage = IWM_RT_SCAN_WAIT;
    for (elapsed = 0; !rt->scan_done; elapsed += 100U) {
        if (elapsed >= IWM_RT_SCAN_TIMEOUT_US) {
            rt->scan_wait_us = elapsed;
            return IWM_ETIMEDOUT;
        }
        rc = iwm_rx_drain(rt->rxq);
        if (rc != IWM_OK && rc != IWM_EFORMAT && rc != IWM_EIO)
            return rc;
        if (!rt->scan_done)
            rt->adapter->ops->delay_us(rt->adapter->os_context, 100U);
    }
    rt->scan_wait_us = elapsed;
    rt->stage = IWM_RT_SCAN_DONE;
    ++rt->scans;
    return IWM_OK;
}

int iwm_rt_fw_scan(struct iwm_rt_fw *rt)
{
    const struct iwm_rt_seq_cmd *c;
    int rc;
    if (!rt || !rt->adapter || !rt->rxq || !rt->tx || !rt->adapter->ops ||
        !rt->adapter->ops->delay_us)
        return IWM_EINVAL;
    if ((rt->stage != IWM_RT_SCAN_DONE && rt->stage != IWM_RT_SCAN_WAIT) ||
        !rt->adapter->mac_valid)
        return IWM_ESTATE;
    c = &iwm_rt_post_cmds[IWM_RT_POST_CMDS - 1U];
    if (!(c->flags & IWM_RT_SEQ_SCAN))
        return IWM_EINVAL;
    scan_reset(rt);
    rt->stage = IWM_RT_SCAN_WAIT;
    rc = send_seq(rt, c);
    if (rc == IWM_OK)
        rc = scan_wait(rt);
    rt->rc = rc;
    return rc;
}

int iwm_rt_fw_scan_probe(struct iwm_rt_fw *rt)
{
    unsigned i;
    int rc = IWM_OK;
    if (!rt || !rt->adapter || !rt->rxq || !rt->tx || !rt->adapter->ops ||
        !rt->adapter->ops->delay_us)
        return IWM_EINVAL;
    if (rt->stage != IWM_RT_DONE || !rt->adapter->mac_valid)
        return rt->rc = IWM_ESTATE;
    scan_reset(rt);
    rt->stage = IWM_RT_POST_SEQ;
    for (i = 0; i < IWM_RT_POST_CMDS; ++i) {
        const struct iwm_rt_seq_cmd *c = &iwm_rt_post_cmds[i];
        rc = send_seq(rt, c);
        if (rc != IWM_OK) {
            if (c->flags & IWM_RT_SEQ_NONFATAL)
                continue;
            goto out;
        }
        if (!(c->flags & IWM_RT_SEQ_SCAN))
            continue;
        rc = scan_wait(rt);
        if (rc != IWM_OK)
            goto out;
    }
    rc = IWM_OK;
out:
    rt->rc = rc;
    return rc;
}
