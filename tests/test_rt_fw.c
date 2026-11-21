/* SPDX-License-Identifier: GPL-2.0-only */
/* Runtime firmware: paging blocks/command, PHY-DB replay, PHY config. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "iwm_rt_fw.h"
#include "iwm_test_beacon.h"

#define CSR_GP_CNTRL         0x024U
#define CSR_HW_REV           0x028U
#define HBUS_TARG_WRPTR      0x460U

struct mock_hw {
    uint32_t regs[2048];
    uint32_t next_phys;
    struct iwm_rx_queue *rxq;
    struct iwm_tx_transport *tx;
    uint16_t rbid;
    uint16_t closed;
    unsigned cmds;
    uint8_t order[64];
    uint8_t groups[64];
    uint16_t phydb_type[64];
    uint16_t phydb_chg[64];
    unsigned phydb;
    uint8_t paging[IWM_PAGING_CMD_SIZE];
    uint8_t phy_cfg[12];
    unsigned aux_ptr_at;      /* command count when HBUS_TARG_WRPTR = 0x100 */
    uint8_t scan_cfg_mac[6];
    uint8_t scan_req_mac[6];
    uint8_t mac_ctx_mac[6];
    uint16_t scan_req_len;
    int no_scan_complete;
};

struct mem_store {
    uint8_t *buf;
    uint32_t len;
};

static int st_resize(void *p, uint32_t n)
{
    struct mem_store *m = (struct mem_store *)p;
    m->buf = (uint8_t *)realloc(m->buf, n ? n : 1U);
    m->len = n;
    return m->buf ? IWM_OK : IWM_ENOMEM;
}

static int st_write(void *p, uint32_t off, const uint8_t *d, uint16_t n)
{
    struct mem_store *m = (struct mem_store *)p;
    memcpy(m->buf + off, d, n);
    return IWM_OK;
}

static int st_read(void *p, uint32_t off, uint8_t *d, uint16_t n)
{
    struct mem_store *m = (struct mem_store *)p;
    if (off > m->len || (uint32_t)n > m->len - off)
        return IWM_EOVERFLOW;
    memcpy(d, m->buf + off, n);
    return IWM_OK;
}

static void st_reset(void *p)
{
    struct mem_store *m = (struct mem_store *)p;
    free(m->buf);
    m->buf = NULL;
    m->len = 0;
}

static const struct iwm_blob_ops store_ops = {
    st_resize, st_write, st_read, st_reset
};

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void push(struct mock_hw *m, uint8_t cmd, uint8_t group, uint16_t seq,
                 const uint8_t *pl, uint32_t len)
{
    uint8_t *b = (uint8_t *)m->rxq->buffers.vaddr +
                 (uint32_t)m->rbid * IWM_RX_BUFFER_SIZE;
    uint32_t *used = (uint32_t *)m->rxq->used_desc.vaddr;
    struct iwm_rx_status *s = (struct iwm_rx_status *)m->rxq->status.vaddr;
    memset(b, 0, 16);
    put32(b, 4U + len);
    b[4] = cmd;
    b[5] = group;
    put16(b + 6, seq);
    if (len)
        memcpy(b + 8, pl, len);
    used[m->closed] = (uint32_t)m->rbid + 1U;
    m->rbid = (uint16_t)((m->rbid + 1U) % 500U);
    m->closed = (uint16_t)((m->closed + 1U) & IWM_RX_QUEUE_MASK);
    s->closed_rb_num = m->closed;
}

static void fw_cmd(struct mock_hw *m, uint16_t idx)
{
    const uint8_t *c = (const uint8_t *)m->tx->cmd_buffers.vaddr +
                       (uint32_t)(idx & (IWM_TX_CMD_WINDOW - 1U)) *
                       IWM_TX_CMD_BUFFER_SIZE;
    uint8_t cmd = c[0], group = c[1];
    uint16_t seq = (uint16_t)(c[2] | (c[3] << 8));
    const uint8_t *pl = c + (group ? 8 : 4);
    uint8_t r[8];
    memset(r, 0, sizeof(r));
    if (m->cmds < sizeof(m->order)) {
        m->order[m->cmds] = cmd;
        m->groups[m->cmds] = group;
    }
    ++m->cmds;
    if (cmd == IWM_CMD_FW_PAGING_BLOCK) {
        assert(group == 1 && c[4] == IWM_PAGING_CMD_SIZE && c[5] == 0);
        memcpy(m->paging, pl, IWM_PAGING_CMD_SIZE);
    } else if (cmd == IWM_CMD_PHY_DB) {
        m->phydb_type[m->phydb] = (uint16_t)(pl[0] | (pl[1] << 8));
        m->phydb_chg[m->phydb] = (uint16_t)(pl[4] | (pl[5] << 8));
        ++m->phydb;
    } else if (cmd == IWM_CMD_PHY_CONFIGURATION) {
        memcpy(m->phy_cfg, pl, 12);
        /* An unrelated notification arrives while waiting. */
        push(m, 0x77, 0, 0x8001, r, 4);
    } else if (group == 1 && cmd == 0x0c) {
        memcpy(m->scan_cfg_mac, pl + 28, 6);
    } else if (group == 0 && cmd == 0x28) {
        memcpy(m->mac_ctx_mac, pl + 16, 6);
    } else if (group == 1 && cmd == 0x0d) {
        static uint8_t f[512];
        static const uint8_t ap[6] = { 0xf0, 0x1d, 0x2d, 0x92, 0x32, 0xe5 };
        uint32_t fl;
        m->scan_req_len = (uint16_t)(c[4] | (c[5] << 8));
        memcpy(m->scan_req_mac, pl + 506, 6);
        push(m, cmd, group, seq, r, 4);
        /* A real-format beacon, a garbage frame, then (unless suppressed)
         * SCAN_COMPLETE_UMAC. */
        fl = tb_build(f, IWM_80211_SUBTYPE_BEACON, ap, "lab-ap", 1, 1, 0x39,
                      IWM_80211_CAP_ESS | IWM_80211_CAP_PRIVACY, 0x101b);
        push(m, IWM_CMD_RX_MPDU, 0, 0x8010, f, fl);
        memset(f, 0x5c, 40);
        push(m, IWM_CMD_RX_MPDU, 0, 0x8011, f, 40);
        if (!m->no_scan_complete) {
            memset(r, 0, sizeof(r));
            r[4] = 1;                    /* last_schedule */
            push(m, IWM_CMD_SCAN_COMPLETE_UMAC, 0, 0x8012, r, 8);
        }
        return;
    }
    push(m, cmd, group, seq, r, 4);
}

static uint32_t mrd(void *p, uint32_t off)
{
    struct mock_hw *m = (struct mock_hw *)p;
    if (off == CSR_GP_CNTRL)
        return m->regs[off >> 2] | 1U;
    return m->regs[(off >> 2) & 2047U];
}

static void mwr(void *p, uint32_t off, uint32_t v)
{
    struct mock_hw *m = (struct mock_hw *)p;
    m->regs[(off >> 2) & 2047U] = v;
    if (off == HBUS_TARG_WRPTR && v == 0x100U)
        m->aux_ptr_at = m->cmds;
    if (off == HBUS_TARG_WRPTR && m->tx && m->tx->scheduler_started &&
        (v >> 8) == 0 && (v & 0xffU) != 0)
        fw_cmd(m, (uint16_t)((v & 0xffU) - 1U));
}

static void mdl(void *p, uint32_t u) { (void)p; (void)u; }

static int malloc_dma(void *p, uint32_t size, uint32_t al, struct iwm_dma_block *b)
{
    struct mock_hw *m = (struct mock_hw *)p;
    void *raw = malloc(size + al);
    if (!raw)
        return IWM_ENOMEM;
    memset(b, 0, sizeof(*b));
    b->alloc_vaddr = raw;
    b->vaddr = (void *)(((size_t)raw + al - 1U) & ~((size_t)al - 1U));
    m->next_phys = IWM_ALIGN_UP(m->next_phys, al);
    b->paddr = m->next_phys;
    m->next_phys += IWM_ALIGN_UP(size, al);
    b->size = size;
    b->alignment = al;
    memset(b->vaddr, 0, size);
    return IWM_OK;
}

static void free_dma(void *p, struct iwm_dma_block *b)
{
    (void)p;
    free(b->alloc_vaddr);
    memset(b, 0, sizeof(*b));
}

static const struct iwm_os_ops ops = {
    mrd, mwr, mdl, malloc_dma, free_dma, NULL, NULL, NULL
};

#define CSS_LEN   100U
#define PAGE_LEN  (9U * 4096U)   /* 2 blocks: 8 pages + 1 page */

static void add_sec(struct iwm_fw_image *im, uint8_t flags, uint32_t dst,
                    uint32_t off, uint32_t len)
{
    struct iwm_fw_section *s = &im->section[im->section_count++];
    s->flags = flags;
    s->dst_addr = dst;
    s->data_offset = off;
    s->data_length = len;
}

static void add_phydb(struct iwm_init_fw *st, uint16_t type, uint16_t chg,
                      uint16_t size)
{
    uint8_t *e = (uint8_t *)st->pool.vaddr + IWM_NVM_POOL_SIZE + st->phydb_used;
    put16(e, type);
    put16(e + 2, size);
    memset(e + 4, 0x40 + type, size);
    put16(e + 4, chg);
    st->phydb_used += 4U + size;
    ++st->phydb_count;
}

static void setup(struct mock_hw *hw, struct iwm_adapter *a,
                  struct iwm_rx_queue *q, struct iwm_tx_transport *tx,
                  struct mem_store *ms, struct iwm_blob *blob,
                  struct iwm_init_fw *init, uint32_t paging)
{
    static const uint8_t mac[6] = { 0xd4, 0x3b, 0x04, 0xc3, 0x04, 0x74 };
    static struct iwm_scan_cache cache;
    struct iwm_fw_image *im;
    uint32_t i;
    memset(hw, 0, sizeof(*hw));
    hw->next_phys = 0x10000000U;
    hw->regs[CSR_HW_REV >> 2] = 0x312U;
    iwm_adapter_init(a, &ops, hw);
    assert(iwm_adapter_probe(a, 0x8086, 0xa370, 0, 54) == IWM_OK);
    assert(iwm_adapter_set_mac(a, mac) == IWM_OK);
    iwm_genmac_bind_scan_cache(a, &cache);
    memset(ms, 0, sizeof(*ms));
    iwm_blob_bind(blob, &store_ops, ms);
    assert(iwm_blob_resize(blob, 64U + CSS_LEN + PAGE_LEN) == IWM_OK);
    for (i = 0; i < 64U + CSS_LEN + PAGE_LEN; ++i)
        ms->buf[i] = (uint8_t)(i * 7U);
    assert(iwm_adapter_set_fw_store(a, blob) == IWM_OK);
    im = &a->firmware.catalog;
    memset(im, 0, sizeof(*im));
    im->phy_config = 0x00330018UL;
    im->calib_flow[IWM_UCODE_TYPE_REGULAR] = 0x11112222UL;
    im->calib_event[IWM_UCODE_TYPE_REGULAR] = 0x33334444UL;
    im->paging_mem_size = paging;
    add_sec(im, IWM_FW_SEC_RUNTIME, 0x404000, 0, 16);
    add_sec(im, IWM_FW_SEC_RUNTIME | IWM_FW_SEC_SEPARATOR,
            IWM_UCODE_SECTION_CPU1_CPU2, 0, 0);
    add_sec(im, IWM_FW_SEC_RUNTIME, 0xc0080000UL, 16, 16);
    add_sec(im, IWM_FW_SEC_INIT, 0x404000, 32, 16);
    add_sec(im, IWM_FW_SEC_RUNTIME | IWM_FW_SEC_SEPARATOR,
            IWM_UCODE_SECTION_PAGING, 0, 0);
    add_sec(im, IWM_FW_SEC_RUNTIME, 0, 64, CSS_LEN);
    add_sec(im, IWM_FW_SEC_RUNTIME, 0x01000000UL, 64U + CSS_LEN, PAGE_LEN);
    iwm_rx_init(q, a);
    assert(iwm_rx_alloc(q) == IWM_OK);
    assert(iwm_rx_program(q) == IWM_OK);
    q->alive_seen = 1;
    iwm_tx_init(tx, a);
    assert(iwm_tx_alloc(tx) == IWM_OK);
    tx->prepared = 1;
    tx->scheduler_started = 1;
    hw->rxq = q;
    hw->tx = tx;
    iwm_init_fw_init(init, a, q, tx);
    assert(malloc_dma(hw, IWM_NVM_POOL_SIZE + IWM_PHYDB_POOL_SIZE, 16U,
                      &init->pool) == IWM_OK);
}

struct mgmt_seen {
    unsigned frames;
    uint8_t first_fc0;
    uint32_t first_len;
};

static void mgmt_cb(void *ctx, const uint8_t *frame, uint32_t len)
{
    struct mgmt_seen *s = (struct mgmt_seen *)ctx;
    if (s->frames == 0) {
        s->first_fc0 = frame[0];
        s->first_len = len;
    }
    ++s->frames;
}

static void teardown(struct iwm_rt_fw *rt, struct iwm_init_fw *init,
                     struct iwm_tx_transport *tx, struct iwm_rx_queue *q,
                     struct iwm_blob *blob)
{
    iwm_rt_fw_free(rt);
    iwm_init_fw_free(init);
    iwm_tx_free(tx);
    iwm_rx_free(q);
    iwm_blob_reset(blob);
}

int main(void)
{
    static struct mock_hw hw;
    static struct iwm_init_fw init;
    static struct iwm_rt_fw rt;
    struct iwm_adapter a;
    struct iwm_rx_queue q;
    struct iwm_tx_transport tx;
    struct mem_store ms;
    struct iwm_blob blob;
    const uint8_t *css, *pg;
    uint32_t i;

    /* Full sequence with paging and a PHY-DB that repeats PAPD group 0. */
    setup(&hw, &a, &q, &tx, &ms, &blob, &init, PAGE_LEN);
    add_phydb(&init, IWM_PHY_DB_CALIB_CHG_PAPD, 0, 8);
    add_phydb(&init, IWM_PHY_DB_CFG, 0, 6);
    add_phydb(&init, IWM_PHY_DB_CALIB_CHG_TXP, 0, 8);
    add_phydb(&init, IWM_PHY_DB_CALIB_NCH, 0, 4);
    add_phydb(&init, IWM_PHY_DB_CALIB_CHG_PAPD, 1, 8);
    add_phydb(&init, IWM_PHY_DB_CALIB_CHG_PAPD, 0, 8);
    iwm_rt_fw_init(&rt, &a, &q, &tx, &init);
    assert(iwm_rt_fw_prepare_paging(&rt) == IWM_OK);
    assert(rt.paging_sep == 4 && rt.css_sec == 5 && rt.data_sec == 6);
    assert(rt.paging_blocks == 2 && rt.pages_last == 1);
    assert(rt.css.size == 4096U && rt.pages.size == 2U * 32768U);
    assert((rt.pages.paddr & 32767U) == 0 && (rt.css.paddr & 4095U) == 0);
    css = (const uint8_t *)rt.css.vaddr;
    pg = (const uint8_t *)rt.pages.vaddr;
    for (i = 0; i < CSS_LEN; ++i)
        assert(css[i] == (uint8_t)((64U + i) * 7U));
    assert(css[CSS_LEN] == 0);
    for (i = 0; i < PAGE_LEN; i += 97U)
        assert(pg[i] == (uint8_t)((64U + CSS_LEN + i) * 7U));

    assert(iwm_rt_fw_run(&rt) == IWM_OK);
    assert(rt.stage == IWM_RT_DONE);
    /* Order: paging (LONG group), LDBG 1/0xf6, SHARED_MEM 2/0x00, SF_CFG,
     * TX ant, PHY-DB x5, PHY cfg (Linux capture #22-#38). */
    assert(hw.cmds == 11U);
    assert(hw.order[0] == IWM_CMD_FW_PAGING_BLOCK && hw.groups[0] == 1);
    assert(hw.order[1] == 0xf6 && hw.groups[1] == 1);
    assert(hw.order[2] == 0x00 && hw.groups[2] == 2);
    assert(hw.order[3] == 0xd1 && hw.groups[3] == 0);
    assert(hw.order[4] == IWM_CMD_TX_ANT_CONFIGURATION && hw.groups[4] == 0);
    for (i = 5; i < 10U; ++i)
        assert(hw.order[i] == IWM_CMD_PHY_DB);
    assert(hw.order[10] == IWM_CMD_PHY_CONFIGURATION);
    assert(rt.cmd_count == 3U && rt.cmd_log[1].id == 0x200 &&
           rt.cmd_log[1].rc == IWM_OK && rt.cmd_log[1].resp_len == 4U);
    /* Paging payload: flags, 2^15 blocks, count, CSS PFN, block PFNs. */
    assert(get32(hw.paging) == (IWM_PAGING_CMD_SECURED |
                                IWM_PAGING_CMD_ENABLED | 1U));
    assert(get32(hw.paging + 4) == 15U && get32(hw.paging + 8) == 2U);
    assert(get32(hw.paging + 12) == rt.css.paddr >> 12);
    assert(get32(hw.paging + 16) == rt.pages.paddr >> 12);
    assert(get32(hw.paging + 20) == (rt.pages.paddr + 32768U) >> 12);
    assert(get32(hw.paging + 24) == 0);
    /* PHY-DB: CFG, NCH, PAPD group 1, PAPD group 0 (latest), TXP. */
    assert(hw.phydb == 5U && rt.phydb_sent == 5U && rt.phydb_skipped == 1U);
    assert(hw.phydb_type[0] == IWM_PHY_DB_CFG);
    assert(hw.phydb_type[1] == IWM_PHY_DB_CALIB_NCH);
    assert(hw.phydb_type[2] == IWM_PHY_DB_CALIB_CHG_PAPD && hw.phydb_chg[2] == 1);
    assert(hw.phydb_type[3] == IWM_PHY_DB_CALIB_CHG_PAPD && hw.phydb_chg[3] == 0);
    assert(hw.phydb_type[4] == IWM_PHY_DB_CALIB_CHG_TXP);
    /* PHY config uses the REGULAR image's triggers. */
    assert(get32(hw.phy_cfg) == 0x00330018UL);
    assert(get32(hw.phy_cfg + 4) == 0x11112222UL);
    assert(get32(hw.phy_cfg + 8) == 0x33334444UL);
    assert(rt.notif_count == 1 && (rt.notif_log[0] & 0xffU) == 0x77);

    /* Post-config Linux sequence and one scan.  The 802.11 frames reach the
     * association layer; the 40-byte garbage frame has no valid descriptor
     * and is not delivered. */
    {
        static struct mgmt_seen seen;
        memset(&seen, 0, sizeof(seen));
        iwm_rt_fw_set_mgmt_cb(&rt, mgmt_cb, &seen);
        assert(iwm_rt_fw_scan_probe(&rt) == IWM_OK);
        assert(seen.frames == 1U && rt.mgmt_frames == 1U &&
               seen.first_fc0 == 0x80 && seen.first_len > 24U);
    }
    assert(rt.stage == IWM_RT_SCAN_DONE && rt.scan_done);
    assert(hw.cmds == 11U + IWM_RT_POST_CMDS);
    for (i = 0; i < IWM_RT_POST_CMDS; ++i) {
        assert(hw.order[11U + i] == (uint8_t)iwm_rt_post_cmds[i].id);
        assert(hw.groups[11U + i] == (uint8_t)(iwm_rt_post_cmds[i].id >> 8));
    }
    assert(rt.cmd_count == 3U + IWM_RT_POST_CMDS);
    /* Aux queue pointer goes out immediately before SCD_QUEUE_CFG (#43). */
    assert(hw.order[11U + 4U] == 0x1d && hw.aux_ptr_at == 11U + 4U);
    /* Adapter MAC patched into scan config, MAC context and probe SA. */
    assert(hw.scan_cfg_mac[0] == 0xd4 && hw.scan_cfg_mac[5] == 0x74);
    assert(hw.mac_ctx_mac[0] == 0xd4 && hw.mac_ctx_mac[5] == 0x74);
    assert(hw.scan_req_mac[0] == 0xd4 && hw.scan_req_mac[5] == 0x74);
    assert(hw.scan_req_len == 1688U);
    assert(rt.scan_complete_len == 8U && rt.scan_complete[4] == 1);
    assert(rt.mpdu_n == 2U && rt.mpdu_len[1] == 40U && rt.mpdu[1][0] == 0x5c);
    /* The beacon lands in the GenMAC cache; the garbage frame is counted. */
    assert(rt.scan_frames == 2U && rt.scan_parsed == 1U && rt.scan_bad == 1U);
    assert(a.scan_cache && a.scan_cache->count == 1U);
    assert(a.scan_cache->result[0].ssid_length == 6 &&
           memcmp(a.scan_cache->result[0].ssid, "lab-ap", 6) == 0);
    assert(a.scan_cache->result[0].privacy == 1 &&
           a.scan_cache->result[0].rssi_dbm == -57 &&
           a.scan_cache->result[0].ds_config_khz == 2412000UL);
    assert(rt.scans == 1U);
    {
        int seen_mpdu = 0, seen_done = 0;
        for (i = 0; i < rt.hist_n; ++i) {
            if (rt.hist[i].key == IWM_CMD_RX_MPDU)
                seen_mpdu = rt.hist[i].count == 2U && rt.hist[i].max_len > 40U;
            if (rt.hist[i].key == IWM_CMD_SCAN_COMPLETE_UMAC)
                seen_done = rt.hist[i].count == 1U;
        }
        assert(seen_mpdu && seen_done);
    }
    /* A second probe needs a fresh runtime configuration. */
    assert(iwm_rt_fw_scan_probe(&rt) == IWM_ESTATE);
    /* A repeat scan sends only SCAN_REQ_UMAC and refreshes the same BSS. */
    i = hw.cmds;
    a.scan_cache->count = 0;
    assert(iwm_rt_fw_scan(&rt) == IWM_OK);
    assert(hw.cmds == i + 1U && hw.order[i] == 0x0d && hw.groups[i] == 1);
    assert(rt.stage == IWM_RT_SCAN_DONE && rt.scans == 2U);
    assert(rt.scan_parsed == 1U && a.scan_cache->count == 1U);
    teardown(&rt, &init, &tx, &q, &blob);

    /* No PAGING TLV: no paging command; missing CFG section is an error. */
    setup(&hw, &a, &q, &tx, &ms, &blob, &init, 0);
    add_phydb(&init, IWM_PHY_DB_CALIB_NCH, 0, 4);
    iwm_rt_fw_init(&rt, &a, &q, &tx, &init);
    assert(iwm_rt_fw_prepare_paging(&rt) == IWM_OK && rt.paging_blocks == 0);
    assert(iwm_rt_fw_run(&rt) == IWM_EFORMAT);
    assert(rt.stage == IWM_RT_PHY_DB && rt.fail_cmd == IWM_CMD_PHY_DB);
    assert(hw.cmds == 4U && hw.order[3] == IWM_CMD_TX_ANT_CONFIGURATION);
    teardown(&rt, &init, &tx, &q, &blob);

    /* No SCAN_COMPLETE: the probe times out in the scan wait. */
    setup(&hw, &a, &q, &tx, &ms, &blob, &init, 0);
    add_phydb(&init, IWM_PHY_DB_CFG, 0, 6);
    add_phydb(&init, IWM_PHY_DB_CALIB_NCH, 0, 4);
    hw.no_scan_complete = 1;
    iwm_rt_fw_init(&rt, &a, &q, &tx, &init);
    assert(iwm_rt_fw_prepare_paging(&rt) == IWM_OK);
    assert(iwm_rt_fw_run(&rt) == IWM_OK);
    assert(iwm_rt_fw_scan_probe(&rt) == IWM_ETIMEDOUT);
    assert(rt.stage == IWM_RT_SCAN_WAIT && !rt.scan_done && rt.mpdu_n == 2U);
    assert(rt.scan_wait_us == IWM_RT_SCAN_TIMEOUT_US);
    /* A scan may be retried after a timeout; it times out again here. */
    assert(iwm_rt_fw_scan(&rt) == IWM_ETIMEDOUT && rt.scans == 0U);
    teardown(&rt, &init, &tx, &q, &blob);

    /* No valid MAC address: refused before any command. */
    setup(&hw, &a, &q, &tx, &ms, &blob, &init, 0);
    add_phydb(&init, IWM_PHY_DB_CFG, 0, 6);
    add_phydb(&init, IWM_PHY_DB_CALIB_NCH, 0, 4);
    iwm_rt_fw_init(&rt, &a, &q, &tx, &init);
    assert(iwm_rt_fw_run(&rt) == IWM_OK);
    a.mac_valid = 0;
    i = hw.cmds;
    assert(iwm_rt_fw_scan_probe(&rt) == IWM_ESTATE && hw.cmds == i);
    assert(iwm_rt_fw_scan(&rt) == IWM_ESTATE && hw.cmds == i);
    teardown(&rt, &init, &tx, &q, &blob);

    /* Paging image shorter than the TLV size is refused. */
    setup(&hw, &a, &q, &tx, &ms, &blob, &init, PAGE_LEN + 4096U);
    iwm_rt_fw_init(&rt, &a, &q, &tx, &init);
    assert(iwm_rt_fw_prepare_paging(&rt) == IWM_EFORMAT);
    teardown(&rt, &init, &tx, &q, &blob);

    /* Not after runtime ALIVE: refused. */
    setup(&hw, &a, &q, &tx, &ms, &blob, &init, 0);
    q.alive_seen = 0;
    iwm_rt_fw_init(&rt, &a, &q, &tx, &init);
    assert(iwm_rt_fw_run(&rt) == IWM_ESTATE);
    teardown(&rt, &init, &tx, &q, &blob);

    puts("runtime firmware tests passed");
    return 0;
}
