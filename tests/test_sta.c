/* SPDX-License-Identifier: GPL-2.0-only */
/* E2: association command replay, management TX on queue 5, teardown. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "iwm_sta.h"
#include "iwm_assoc.h"
#include "iwm_data.h"

#define CSR_GP_CNTRL         0x024U
#define CSR_HW_REV           0x028U
#define HBUS_TARG_WRPTR      0x460U

static const uint8_t OWN[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
static const uint8_t AP[6]  = { 0x0a, 0xbb, 0xcc, 0xdd, 0xee, 0xf0 };

struct mock_hw {
    uint32_t regs[2048];
    uint32_t next_phys;
    struct iwm_rx_queue *rxq;
    struct iwm_tx_transport *tx;
    uint16_t rbid;
    uint16_t closed;
    unsigned cmds;
    uint16_t ids[160];
    uint32_t wrptr_log[200];
    unsigned wrptr_n;
    uint8_t last_phy[40];
    uint8_t last_mac[148];
    uint8_t te_remove[36];
    uint8_t scd[2][12];
    unsigned scd_n;
    uint16_t fail_id;          /* command that gets no response */
    int no_te_notif;
    unsigned frames;
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
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

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
    uint16_t id = (uint16_t)(((uint16_t)group << 8) | cmd);
    const uint8_t *pl = c + (group ? 8 : 4);
    uint8_t r[16];
    memset(r, 0, sizeof(r));
    if (m->cmds < 160)
        m->ids[m->cmds] = id;
    ++m->cmds;
    if (id == m->fail_id)
        return;                                /* no response: timeout */
    if (id == 0x08)
        memcpy(m->last_phy, pl, 40);
    if (id == 0x28)
        memcpy(m->last_mac, pl, 148);
    if (id == 0x1d && pl[4] && m->scd_n < 2)
        memcpy(m->scd[m->scd_n++], pl, 12);
    if (id == 0x29 && pl[4] == 3)
        memcpy(m->te_remove, pl, 36);
    if (id == 0x29 && pl[4] == 1) {
        put32(r + 8, 0x2e01);                  /* unique id */
        push(m, cmd, group, seq, r, 16);
        if (!m->no_te_notif) {
            memset(r, 0, sizeof(r));
            r[0] = 0x55;
            push(m, IWM_STA_CMD_TIME_EVENT_NOTIF, 0, 0x8020, r, 16);
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
    if (off != HBUS_TARG_WRPTR || !m->tx || !m->tx->scheduler_started)
        return;
    if (m->wrptr_n < 200)
        m->wrptr_log[m->wrptr_n++] = v;
    if ((v >> 8) == 0 && (v & 0xffU) != 0) {
        fw_cmd(m, (uint16_t)((v & 0xffU) - 1U));
    } else if ((v >> 8) == IWM_TX_MGMT_QUEUE && (v & 0xffU) != 0) {
        uint8_t r[48];
        memset(r, 0, sizeof(r));
        r[0] = 1;                               /* frame count */
        ++m->frames;
        push(m, IWM_TX_CMD_TX, 0,
             (uint16_t)((IWM_TX_MGMT_QUEUE << 8) | ((v & 0xffU) - 1U)), r, 48);
    }
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

static struct mock_hw hw;
static struct iwm_adapter a;
static struct iwm_rx_queue q;
static struct iwm_tx_transport tx;
static struct iwm_rt_fw rt;
static struct iwm_sta sta;

static void setup(void)
{
    memset(&hw, 0, sizeof(hw));
    hw.next_phys = 0x10000000U;
    hw.regs[CSR_HW_REV >> 2] = 0x312U;
    iwm_adapter_init(&a, &ops, &hw);
    assert(iwm_adapter_probe(&a, 0x8086, 0xa370, 0, 54) == IWM_OK);
    assert(iwm_adapter_set_mac(&a, OWN) == IWM_OK);
    iwm_rx_init(&q, &a);
    assert(iwm_rx_alloc(&q) == IWM_OK);
    assert(iwm_rx_program(&q) == IWM_OK);
    q.alive_seen = 1;
    iwm_tx_init(&tx, &a);
    assert(iwm_tx_alloc(&tx) == IWM_OK);
    tx.prepared = 1;
    tx.scheduler_started = 1;
    hw.rxq = &q;
    hw.tx = &tx;
    iwm_rt_fw_init(&rt, &a, &q, &tx, NULL);
    q.notif_cb = NULL;
    iwm_sta_init(&sta, &rt, &tx);
    iwm_rt_fw_set_notif_hook(&rt, iwm_sta_notif, &sta);
    assert(q.notif_cb != NULL && q.notif_ctx == &rt);
}

static struct iwm_scan_result bss_at(uint32_t khz)
{
    struct iwm_scan_result b;
    memset(&b, 0, sizeof(b));
    memcpy(b.bssid, AP, 6);
    memcpy(b.ssid, "lab", 3);
    b.ssid_length = 3;
    b.ds_config_khz = khz;
    b.beacon_period = 200;
    b.infrastructure_mode = IWM_INFRASTRUCTURE_BSS;
    return b;
}

static void teardown_all(void)
{
    iwm_tx_free(&tx);
    iwm_rx_free(&q);
}

int main(void)
{
    static const uint8_t auth[30] = {
        0xb0, 0, 0, 0, 0x0a, 0xbb, 0xcc, 0xdd, 0xee, 0xf0,
        0x02, 0x11, 0x22, 0x33, 0x44, 0x55, 0x0a, 0xbb, 0xcc, 0xdd, 0xee, 0xf0,
        0, 0, 0, 0, 1, 0, 0, 0
    };
    static uint8_t buf[160];
    struct iwm_scan_result bss;
    struct iwm_tx_fq *fq;
    const uint8_t *tfd, *b, *bct;
    uint16_t seq = 0;
    unsigned i, before;

    /* ---- channel helper and band-limited selection */
    assert(iwm_sta_channel_24(2412000UL) == 1 && iwm_sta_channel_24(2462000UL) == 11);
    assert(iwm_sta_channel_24(2484000UL) == 14);
    assert(iwm_sta_channel_24(5180000UL) == 0 && iwm_sta_channel_24(2413000UL) == 0);
    {
        static struct iwm_scan_cache cache;
        struct iwm_scan_result sel;
        memset(&cache, 0, sizeof(cache));
        cache.result[0] = bss_at(5320000UL);
        cache.result[0].rssi_dbm = -40;
        cache.result[1] = bss_at(2412000UL);
        cache.result[1].rssi_dbm = -60;
        cache.count = 2;
        assert(iwm_assoc_select(&cache, (const uint8_t *)"lab", 3, NULL, &sel) == IWM_OK &&
               sel.ds_config_khz == 5320000UL);
        assert(iwm_assoc_select_band(&cache, (const uint8_t *)"lab", 3, NULL,
                                     3000000UL, &sel) == IWM_OK &&
               sel.ds_config_khz == 2412000UL);
        cache.count = 1;
        assert(iwm_assoc_select_band(&cache, (const uint8_t *)"lab", 3, NULL,
                                     3000000UL, &sel) == IWM_ENODEV);
    }

    /* ---- table patching */
    setup();
    memcpy(sta.mac, OWN, 6);
    memcpy(sta.bssid, AP, 6);
    sta.channel = 6;
    sta.beacon_int = 200;
    sta.aid = 0x1234;
    sta.te_id = 0xabcd;
    assert(iwm_sta_build(&sta, &iwm_sta_seq_pre[0], buf) == IWM_OK);      /* PHY */
    assert(get32(buf + 16) == 6 && buf[20] == 1);
    assert(iwm_sta_build(&sta, &iwm_sta_seq_pre[5], buf) == IWM_OK);      /* MAC */
    assert(memcmp(buf + 16, OWN, 6) == 0 && memcmp(buf + 24, AP, 6) == 0);
    assert(get32(buf + 0x74) == 200 && get32(buf + 0x88) == 0);      /* not assoc */
    assert(iwm_sta_build(&sta, &iwm_sta_seq_up[0], buf) == IWM_OK);
    assert(get32(buf + 0x88) == 0x1234 && get32(buf + 0x7c) == 200);
    assert(iwm_sta_build(&sta, &iwm_sta_seq_pre[7], buf) == IWM_OK);      /* ADD_STA */
    assert(memcmp(buf + 8, AP, 6) == 0);
    assert(iwm_sta_build(&sta, &iwm_sta_seq_te[0], buf) == IWM_OK);
    assert(get32(buf + 8) == 0xabcd && buf[4] == 3);
    assert(iwm_sta_build(&sta, &iwm_sta_seq_auth[1], buf) == IWM_OK);     /* SCAN_CFG */
    assert(memcmp(buf + 28, OWN, 6) == 0);
    /* Disconnect MAC contexts carry no beacon timestamps. */
    assert(iwm_sta_build(&sta, &iwm_sta_seq_down[0], buf) == IWM_OK);
    for (i = 0x68; i < 0x74; ++i)
        assert(buf[i] == 0);
    assert(get32(buf + 0x8c) == 0 && get32(buf + 0x64) == 1);

    /* ---- filter_or: ORed into filter_flags of every replayed MAC context and
     * nothing else; captured values are 0x44 (before association) and 0x04
     * (after: beacons are filtered by the firmware). */
    sta.filter_or = IWM_MAC_FILTER_IN_PROMISC | IWM_MAC_FILTER_IN_CONTROL_AND_MGMT;
    assert(iwm_sta_build(&sta, &iwm_sta_seq_pre[5], buf) == IWM_OK);       /* MAC */
    assert(get32(buf + IWM_STA_MAC_FILTER_OFF) == 0x47U);
    assert(memcmp(buf + 16, OWN, 6) == 0 && get32(buf + 0x74) == 200);
    assert(iwm_sta_build(&sta, &iwm_sta_seq_up[7], buf) == IWM_OK);        /* c82 */
    assert(get32(buf + IWM_STA_MAC_FILTER_OFF) == 0x07U);
    assert(iwm_sta_build(&sta, &iwm_sta_seq_pre[7], buf) == IWM_OK);       /* ADD_STA */
    assert(memcmp(buf + 8, AP, 6) == 0);
    sta.filter_or = 0;
    assert(iwm_sta_build(&sta, &iwm_sta_seq_up[7], buf) == IWM_OK);
    assert(get32(buf + IWM_STA_MAC_FILTER_OFF) == 0x04U);

    /* ---- 5 GHz refused before any command */
    bss = bss_at(5320000UL);
    assert(iwm_sta_prepare(&sta, &bss, OWN) == IWM_ENOTSUP && hw.cmds == 0);
    assert(sta.phase == IWM_STP_IDLE);

    /* ---- full exchange */
    bss = bss_at(2437000UL);
    assert(iwm_sta_prepare(&sta, &bss, OWN) == IWM_OK);
    assert(sta.phase == IWM_STP_PREPARED && hw.cmds == IWM_STA_PRE);
    for (i = 0; i < IWM_STA_PRE; ++i)
        assert(hw.ids[i] == iwm_sta_seq_pre[i].id && sta.log[i].rc == IWM_OK);
    assert(sta.channel == 6 && get32(hw.last_phy + 16) == 6);
    assert(sta.te_active && sta.te_id == 0x2e01 && sta.te_started &&
           sta.te_notif[0] == 0x55);
    /* Queue 5 pointer reset precedes SCD_QUEUE_CFG (queue 5, TID 8, FIFO 3). */
    assert(hw.scd_n == 1 && hw.scd[0][3] == 5 && hw.scd[0][2] == 8 &&
           hw.scd[0][6] == 3);
    for (i = 0; i < hw.wrptr_n && hw.wrptr_log[i] != 0x500U; ++i)
        ;
    assert(i < hw.wrptr_n && hw.wrptr_log[i + 1] == 11U);       /* then cmd 11 */

    /* Authentication frame on queue 5. */
    assert(iwm_sta_tx_mgmt(&sta, auth, sizeof(auth)) == IWM_OK);
    fq = iwm_tx_fq_get(&tx, IWM_TX_MGMT_QUEUE);
    assert(fq && fq->sent == 1 && hw.frames == 1);
    b = (const uint8_t *)fq->bufs.vaddr;
    assert(b[0] == IWM_TX_CMD_TX && b[1] == 0 && get16(b + 2) == 0x0500);
    assert(get16(b + 4) == 30 && get16(b + 6) == 0x0c00);        /* len, MH 12 */
    assert(get16(b + 4 + IWM_STA_TX_PM_TIMEOUT_OFF) == 2);
    assert(get32(b + 4 + 44) == fq->first_tb.paddr + 12U);       /* scratch */
    assert(b[4 + 0x31] == 0x3c && b[4 + 0x32] == 0x0f && b[4 + 0x33] == 8);
    assert(memcmp(b + 60, auth, 30) == 0);
    tfd = (const uint8_t *)fq->tfds.vaddr;
    assert(tfd[3] == 2 && get32(tfd + 4) == fq->first_tb.paddr);
    assert(get16(tfd + 8) == (20U << 4));
    assert(get32(tfd + 10) == fq->bufs.paddr + 20U && get16(tfd + 14) == (70U << 4));
    assert(memcmp(fq->first_tb.vaddr, b, 20) == 0);
    bct = (const uint8_t *)tx.scd_bc_table.vaddr +
          IWM_TX_MGMT_QUEUE * IWM_TX_BC_ENTRIES * IWM_TX_BC_ENTRY_SIZE;
    assert(get16(bct) == 10 && get16(bct + 2U * 256U) == 10);     /* (30+8)/4 */
    /* The TX response reclaims the slot. */
    (void)iwm_rx_drain(&q);
    assert(fq->read == 1 && fq->done == 1 && sta.tx_resp == 1 &&
           sta.txresp_seq[0] == 0x0500 && sta.txresp[0][0] == 1);

    /* Association request: post-auth commands first, pm timeout 3. */
    before = hw.cmds;
    {
        static uint8_t ar[46];
        memcpy(ar, auth, 24);
        ar[0] = 0x00;
        assert(iwm_sta_tx_mgmt(&sta, ar, sizeof(ar)) == IWM_OK);
    }
    assert(hw.cmds == before + IWM_STA_AUTHED && sta.phase == IWM_STP_AUTHED);
    assert(hw.ids[before] == 0x9c && hw.ids[before + 1] == 0x10c);
    b = (const uint8_t *)fq->bufs.vaddr + IWM_TX_FRAME_BUF_SIZE;
    assert(get16(b + 2) == 0x0501 && get16(b + 4 + IWM_STA_TX_PM_TIMEOUT_OFF) == 3);
    /* A second request does not repeat them. */
    before = hw.cmds;
    {
        static uint8_t ar[46];
        assert(iwm_sta_tx_mgmt(&sta, ar, sizeof(ar)) == IWM_OK && hw.cmds == before);
    }

    /* Associated: post-association set with the AID, then TE removal. */
    before = hw.cmds;
    assert(iwm_sta_up(&sta, 5) == IWM_OK && sta.phase == IWM_STP_UP);
    assert(hw.cmds == before + IWM_STA_UP + 1U);
    assert(hw.ids[before + IWM_STA_UP] == 0x29);
    assert(get32(hw.te_remove + 8) == 0x2e01 && !sta.te_active);
    assert(hw.scd_n == 2 && hw.scd[1][3] == 4 && hw.scd[1][6] == 1);
    assert(get32(hw.last_mac + 0x88) == 5 && get32(hw.last_mac + 0x74) == 200);
    assert(get32(hw.last_phy + 16) == 6);

    /* Data path on queue 4 (enabled by the post-association set). */
    {
        static struct iwm_data d;
        static uint8_t eth[60];
        struct iwm_tx_fq *dq = iwm_tx_fq_get(&tx, IWM_TX_DATA_QUEUE);
        const uint8_t *db;
        iwm_data_init(&d, &tx);
        memset(eth, 0x5a, sizeof(eth));
        eth[12] = 0x08;
        eth[13] = 0x06;
        assert(iwm_data_tx(&d, eth, 60) == IWM_ESTATE);   /* not linked */
        iwm_data_link(&d, OWN, AP);
        assert(iwm_data_tx(&d, eth, 60) == IWM_OK && d.tx_ok == 1 && d.seq == 1);
        assert(dq->sent == 1 && dq->write == 1);
        db = (const uint8_t *)dq->bufs.vaddr;
        assert(db[0] == IWM_TX_CMD_TX && get16(db + 2) == 0x0400);
        assert(get16(db + 4) == 80U && get16(db + 6) == 0x2d00);  /* len, MH */
        assert(get32(db + 8) == 0x818U && db[4 + 16] == 0);         /* flags, sta */
        assert(db[4 + 0x31] == 0x3c && db[4 + 0x32] == 0x0f && db[4 + 0x33] == 0);
        assert(db[60] == 0x88 && db[61] == 0x01 && memcmp(db + 64, AP, 6) == 0);
        assert(db[86] == 0 && db[87] == 0);                         /* pad */
        assert(db[88] == 0xaa && db[94] == 0x08 && db[95] == 0x06);
        assert(iwm_data_tx(&d, eth, 60) == IWM_OK && d.seq == 2);
        db += IWM_TX_FRAME_BUF_SIZE;
        assert(db[60 + 22] == 0x10);                                /* seq 1 */
    }

    /* Action frame (ADDBA response) while associated: queue 5, Linux
     * pm_frame_timeout 0 (association request 3, authentication 2). */
    {
        static uint8_t act[33];
        const uint8_t *ab;
        memset(act, 0, sizeof(act));
        act[0] = 0xd0;
        memcpy(act + 4, AP, 6);
        memcpy(act + 10, OWN, 6);
        memcpy(act + 16, AP, 6);
        act[24] = 3; act[25] = 1; act[26] = 0xaa; act[27] = 37;
        before = (unsigned)fq->sent;
        assert(iwm_sta_tx_mgmt(&sta, act, sizeof(act)) == IWM_OK);
        assert(fq->sent == before + 1U);
        ab = (const uint8_t *)fq->bufs.vaddr +
             (uint32_t)(before & (IWM_TX_FRAME_WINDOW - 1U)) * IWM_TX_FRAME_BUF_SIZE;
        assert(get16(ab + 4 + IWM_STA_TX_PM_TIMEOUT_OFF) == 0);
        assert(get16(ab + 4) == 33U && ab[60] == 0xd0 && ab[60 + 25] == 1);
        (void)iwm_rx_drain(&q);                  /* its TX response */
    }

    /* Disconnect: full sequence minus the two skipped RX-queue syncs (0x502);
     * queues back to 0. */
    before = hw.cmds;
    iwm_sta_teardown(&sta);
    assert(hw.cmds == before + IWM_STA_DOWN - 2U && sta.phase == IWM_STP_IDLE);
    {
        unsigned k = 0, skipped = 0;
        for (i = 0; i < IWM_STA_DOWN; ++i) {
            if (iwm_sta_seq_down[i].flags & IWM_SS_SKIP) {
                assert(iwm_sta_seq_down[i].id == 0x502U);
                ++skipped;
                continue;
            }
            assert(hw.ids[before + k] == iwm_sta_seq_down[i].id);
            ++k;
        }
        assert(skipped == 2U && k == IWM_STA_DOWN - 2U);
    }
    for (i = 0; i < hw.cmds; ++i)
        assert(hw.ids[i] != 0x502U);
    assert(fq->read == 0 && fq->write == 0);
    iwm_sta_teardown(&sta);                          /* idempotent */
    assert(hw.cmds == before + IWM_STA_DOWN - 2U);

    /* Unsupported transitions. */
    assert(iwm_sta_tx_mgmt(&sta, auth, sizeof(auth)) == IWM_ESTATE);
    assert(iwm_sta_up(&sta, 1) == IWM_ESTATE);
    teardown_all();

    /* ---- failure inside the pre-auth set: short teardown */
    setup();
    hw.fail_id = 0x18;                                /* first ADD_STA times out */
    bss = bss_at(2412000UL);
    assert(iwm_sta_prepare(&sta, &bss, OWN) == IWM_ETIMEDOUT);
    assert(sta.phase == IWM_STP_FAILED && sta.fail_id == 0x18);
    before = hw.cmds;
    hw.fail_id = 0;
    iwm_sta_teardown(&sta);
    assert(hw.cmds == before + IWM_STA_ABORT && sta.phase == IWM_STP_IDLE);
    assert(hw.ids[before] == 0x1e && hw.ids[before + IWM_STA_ABORT - 1U] == 0x08);
    teardown_all();

    /* ---- no time-event notification: logged wait, not fatal; short
     * teardown removes the still-active time event first */
    setup();
    hw.no_te_notif = 1;
    bss = bss_at(2412000UL);
    assert(iwm_sta_prepare(&sta, &bss, OWN) == IWM_OK);
    assert(!sta.te_started && sta.te_wait_us == IWM_STA_TE_WAIT_US && sta.te_active);
    before = hw.cmds;
    iwm_sta_teardown(&sta);
    assert(hw.cmds == before + 1U + IWM_STA_ABORT && hw.ids[before] == 0x29);
    teardown_all();

    /* ---- d20: a command timeout ends the set at once; two in a row mark the
     * firmware dead and further commands fail without waiting. */
    setup();
    bss = bss_at(2412000UL);
    assert(iwm_sta_prepare(&sta, &bss, OWN) == IWM_OK);
    assert(iwm_sta_tx_mgmt(&sta, auth, sizeof(auth)) == IWM_OK);
    {
        static uint8_t ar[46];
        memcpy(ar, auth, 24);
        ar[0] = 0x00;
        assert(iwm_sta_tx_mgmt(&sta, ar, sizeof(ar)) == IWM_OK);
    }
    assert(iwm_sta_up(&sta, 5) == IWM_OK && sta.phase == IWM_STP_UP);
    assert(rt.cmd_timeouts == 0 && !rt.dead);
    before = hw.cmds;
    hw.fail_id = 0xa9;                       /* 2nd teardown command: no answer */
    iwm_sta_teardown(&sta);
    assert(sta.phase == IWM_STP_IDLE && sta.fail_id == 0xa9);
    assert(hw.cmds == before + 2U);          /* 0x28 answered, 0xa9 timed out, stop */
    assert(rt.cmd_timeouts == 1 && rt.consec_timeouts == 1 && !rt.dead);
    before = hw.cmds;
    assert(iwm_rt_fw_send(&rt, 0xa9, NULL, 0, NULL, 0, NULL) == IWM_ETIMEDOUT);
    assert(rt.cmd_timeouts == 2 && rt.dead && hw.cmds == before + 1U);
    before = hw.cmds;
    assert(iwm_rt_fw_send(&rt, 0x03, NULL, 0, NULL, 0, NULL) == IWM_EIO);
    assert(hw.cmds == before && rt.cmd_timeouts == 2);   /* nothing was queued */
    hw.fail_id = 0;
    assert(iwm_sta_prepare(&sta, &bss, OWN) == IWM_EIO && hw.cmds == before);
    teardown_all();
    /* A response resets the run of timeouts. */
    setup();
    hw.fail_id = 0x03;
    assert(iwm_rt_fw_send(&rt, 0x03, NULL, 0, NULL, 0, NULL) == IWM_ETIMEDOUT);
    hw.fail_id = 0;
    assert(iwm_rt_fw_send(&rt, 0x04, NULL, 0, NULL, 0, NULL) == IWM_OK);
    assert(rt.consec_timeouts == 0 && rt.cmd_timeouts == 1 && !rt.dead);
    hw.fail_id = 0x03;
    assert(iwm_rt_fw_send(&rt, 0x03, NULL, 0, NULL, 0, NULL) == IWM_ETIMEDOUT);
    assert(!rt.dead);
    teardown_all();

    /* ---- d27: stale TX responses must not wedge a queue.  After a reset
     * (write = read = 0) three late responses for slots 2..4 arrive; the next
     * frame must still go out (it failed with rc -4 before). */
    setup();
    {
        static uint8_t f[64], tc[IWM_TX_TXCMD_SIZE];
        struct iwm_tx_fq *sq;
        memset(f, 0, sizeof(f));
        f[0] = 0x88;
        sq = iwm_tx_fq_get(&tx, IWM_TX_DATA_QUEUE);
        assert(iwm_tx_queue_set_ptr(&tx, IWM_TX_DATA_QUEUE, 0) == IWM_OK);
        iwm_tx_frame_done(&tx, 0x0402);
        iwm_tx_frame_done(&tx, 0x0403);
        iwm_tx_frame_done(&tx, 0x0404);
        assert(sq->read == 0 && sq->done == 0 && sq->stale == 3U);
        assert(iwm_tx_send_frame(&tx, IWM_TX_DATA_QUEUE, tc, f, 60, 26, &seq) == IWM_OK);
        assert(iwm_tx_send_frame(&tx, IWM_TX_DATA_QUEUE, tc, f, 60, 26, &seq) == IWM_OK);
        /* In flight: slots 0 and 1.  A response for slot 1 reclaims both, a
         * second one for slot 1 and one for slot 2 do not. */
        iwm_tx_frame_done(&tx, 0x0401);
        assert(sq->read == 2 && sq->done == 1U);
        iwm_tx_frame_done(&tx, 0x0401);
        iwm_tx_frame_done(&tx, 0x0402);
        assert(sq->read == 2 && sq->done == 1U && sq->stale == 5U);
        /* Index wrap: reclaim across 255 -> 0. */
        sq->read = 254; sq->write = 2; sq->done = 0;
        iwm_tx_frame_done(&tx, 0x0400);
        assert(sq->read == 1 && sq->done == 1U);
        iwm_tx_frame_done(&tx, 0x0200 + 0x04);     /* unknown queue: ignored */
        iwm_tx_frame_done(&tx, 0x0900);
        assert(sq->read == 1 && sq->done == 1U);
    }
    teardown_all();

    /* ---- transport: QoS data header padding, window, bad queue */
    setup();
    {
        static uint8_t f[64], tc[IWM_TX_TXCMD_SIZE];
        memset(f, 0, sizeof(f));
        f[0] = 0x88;
        assert(iwm_tx_queue_set_ptr(&tx, IWM_TX_DATA_QUEUE, 0) == IWM_OK);
        assert(iwm_tx_send_frame(&tx, IWM_TX_DATA_QUEUE, tc, f, 60, 26, &seq) == IWM_OK);
        fq = iwm_tx_fq_get(&tx, IWM_TX_DATA_QUEUE);
        b = (const uint8_t *)fq->bufs.vaddr;
        assert(seq == 0x0400 && get16(b + 6) == 0x2d00);          /* MH 13 + pad */
        assert(get16((const uint8_t *)fq->tfds.vaddr + 14) == ((4U + 56U + 62U - 20U) << 4));
        for (i = 1; i < IWM_TX_FRAME_WINDOW - 1U; ++i)
            assert(iwm_tx_send_frame(&tx, IWM_TX_DATA_QUEUE, tc, f, 60, 26, &seq) == IWM_OK);
        assert(iwm_tx_send_frame(&tx, IWM_TX_DATA_QUEUE, tc, f, 60, 26, &seq) == IWM_EBUSY);
        iwm_tx_frame_done(&tx, 0x0400);
        assert(iwm_tx_send_frame(&tx, IWM_TX_DATA_QUEUE, tc, f, 60, 26, &seq) == IWM_OK);
        assert(iwm_tx_send_frame(&tx, 3, tc, f, 60, 26, &seq) == IWM_EINVAL);
        assert(iwm_tx_send_frame(&tx, IWM_TX_DATA_QUEUE, tc, f, 20, 24, &seq) == IWM_EINVAL);
        assert(iwm_tx_send_frame(&tx, IWM_TX_DATA_QUEUE, tc, f, 2000, 24, &seq) == IWM_EOVERFLOW);
    }
    teardown_all();

    puts("station tests passed");
    return 0;
}
