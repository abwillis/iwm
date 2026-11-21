/* os2/iwm_d32_fw.c -- ring-0 firmware staging, category 0x91 IOCTL,
 * and M2 FH service-channel INIT loader for IWMFWLD.EXE.
 * Declarations: Dev32lib.h, Dev32Help.h, strategy.h (OS2Stuff/Drv32). */
#include "Dev32lib.h"
#include "iwm_hw.h"
#include "iwm_fw_loader.h"
#include "iwm_rx.h"
#include "iwm_tx.h"
#include "iwm_init_fw.h"
#include "iwm_rt_fw.h"
#include "iwm_assoc.h"
#include "iwm_sta.h"
#include "iwm_data.h"
#include "iwm_ndis.h"
#include "iwm_os2.h"
#include "iwm_d32.h"

#define IWM_VENDOR  0x8086
#define IWM_DEVICE  0xA370
/* Linux cfg/9000.c mac_addr_from_csr. */
#define IWM_9000_MAC_CSR_BASE 0x380UL

/* memcpy/memset come from Dev32lib.h (compiler intrinsics); the core files
 * use them the same way, so nothing is supplied here. */

/* Firmware store: fixed, contiguous memory with a flat address, so the image
 * is directly readable now and its physical address is known for the later
 * FH DMA step. */
static struct {
    void *base;
    ULONG phys;
    ULONG size;
} g_fw;

static int fws_resize(void *c, uint32_t n)
{
    (void)c;
    if (g_fw.base) {
        D32FreeMem(g_fw.base);
        g_fw.base = 0;
        g_fw.phys = 0;
        g_fw.size = 0;
    }
    if (n == 0)
        return IWM_OK;
    g_fw.base = D32AllocMem(n, &g_fw.phys, 0);
    if (!g_fw.base) {
        dprintf(0, "iwm: fw store alloc of %lu bytes failed\n", (ULONG)n);
        return IWM_ENOMEM;
    }
    g_fw.size = n;
    dprintf(0, "iwm: fw store %lu bytes at phys %08lx\n", (ULONG)n, g_fw.phys);
    return IWM_OK;
}

static int fws_write(void *c, uint32_t off, const uint8_t *d, uint16_t n)
{
    (void)c;
    if (!g_fw.base || off > g_fw.size || (ULONG)n > g_fw.size - off)
        return IWM_EOVERFLOW;
    memcpy((uint8_t *)g_fw.base + off, d, n);
    return IWM_OK;
}

static int fws_read(void *c, uint32_t off, uint8_t *d, uint16_t n)
{
    (void)c;
    if (!g_fw.base || off > g_fw.size || (ULONG)n > g_fw.size - off)
        return IWM_EOVERFLOW;
    memcpy(d, (uint8_t *)g_fw.base + off, n);
    return IWM_OK;
}

static void fws_reset(void *c)
{
    (void)c;
    if (g_fw.base)
        D32FreeMem(g_fw.base);
    g_fw.base = 0;
    g_fw.phys = 0;
    g_fw.size = 0;
}

static const struct iwm_blob_ops g_store_ops = {
    fws_resize, fws_write, fws_read, fws_reset
};

/* CSR access through the window iwm_d32_m1() mapped. */
static uint32_t d_rd(void *c, uint32_t off)
{
    volatile unsigned long *csr = iwm_d32_csr();

    (void)c;
    return csr ? csr[off / 4] : 0xFFFFFFFFUL;
}

/* ---- register-write coverage trace (diagnostic) -------------------------
 * Every CSR write the core makes passes through d_wr(). Writes to the PRPH
 * and MEM gateways are folded into one record per target register, so the
 * table says what was written, not how. One row per distinct register:
 * first value, last value, write count and the ordinal of the first write.
 * "mark" is the ordinal of the first FH_UCODE_LOAD_STATUS = 0xffffffff
 * write, i.e. the end of the firmware load; the table is frozen there, so it
 * holds exactly the writes up to the end of the load ("post" counts the rest).
 * iwm_tracediff.py compares the dump with a Linux iwlwifi trace of the same
 * card. Task-level writes only: the interrupt handler's acknowledge writes
 * are not recorded. */
#define IWM_TR_ROWS      256
#define IWM_TR_PRPH      0x01000000UL
#define IWM_TR_MEM       0x02000000UL
#define IWM_TR_PAD       0xFFFFFFFFUL

struct iwm_tr_row {
    ULONG key;
    ULONG first;
    ULONG last;
    ULONG count;
    ULONG seq;
};

static struct iwm_tr_row g_tr[IWM_TR_ROWS];
static unsigned g_tr_n;
static ULONG g_tr_total;
static ULONG g_tr_lost;
static ULONG g_tr_mark;
static ULONG g_tr_post;
static ULONG g_tr_waddr;
static ULONG g_tr_maddr;

static void tr_note(ULONG key, ULONG val)
{
    unsigned i;

    ++g_tr_total;
    for (i = 0; i < g_tr_n; ++i) {
        if (g_tr[i].key == key) {
            g_tr[i].last = val;
            ++g_tr[i].count;
            return;
        }
    }
    if (g_tr_n >= IWM_TR_ROWS) {
        ++g_tr_lost;
        return;
    }
    g_tr[g_tr_n].key = key;
    g_tr[g_tr_n].first = val;
    g_tr[g_tr_n].last = val;
    g_tr[g_tr_n].count = 1;
    g_tr[g_tr_n].seq = g_tr_total;
    ++g_tr_n;
}

static void iwm_tr_w(uint32_t off, uint32_t v)
{
    if (g_tr_mark) {                  /* load finished: table is frozen */
        ++g_tr_post;
        return;
    }
    switch (off) {
    case 0x444:                       /* HBUS_TARG_PRPH_WADDR */
        g_tr_waddr = v & 0x000fffffUL;
        return;
    case 0x448:                       /* HBUS_TARG_PRPH_RADDR: read gateway */
        return;
    case 0x44c:                       /* HBUS_TARG_PRPH_WDAT */
        tr_note(IWM_TR_PRPH | g_tr_waddr, v);
        return;
    case 0x410:                       /* HBUS_TARG_MEM_WADDR */
        g_tr_maddr = v;
        return;
    case 0x418:                       /* HBUS_TARG_MEM_WDAT */
        tr_note(IWM_TR_MEM | (g_tr_maddr & 0x00ffffffUL), v);
        return;
    default:
        tr_note((ULONG)off, v);
        break;
    }
    if (off == 0x1af0 && v == 0xFFFFFFFFUL && g_tr_mark == 0)
        g_tr_mark = g_tr_total;
}

static const char *tr_kind(ULONG key)
{
    if (key == IWM_TR_PAD)
        return "-";
    if (key & IWM_TR_PRPH)
        return "p";
    if (key & IWM_TR_MEM)
        return "m";
    return "c";
}

static void iwm_d32_trace_dump(void)
{
    static const struct iwm_tr_row pad = { IWM_TR_PAD, 0, 0, 0, 0 };
    unsigned i;

    dprintf(0, "iwm: wtrace rows %u total %lu lost %lu mark %lu post %lu\n",
            g_tr_n, g_tr_total, g_tr_lost, g_tr_mark, g_tr_post);
    for (i = 0; i < g_tr_n; i += 3) {
        const struct iwm_tr_row *r0 = &g_tr[i];
        const struct iwm_tr_row *r1 = (i + 1 < g_tr_n) ? &g_tr[i + 1] : &pad;
        const struct iwm_tr_row *r2 = (i + 2 < g_tr_n) ? &g_tr[i + 2] : &pad;

        dprintf(0,
            "iwm: wt %s%lx=%08lx/%08lx*%lu@%lu %s%lx=%08lx/%08lx*%lu@%lu %s%lx=%08lx/%08lx*%lu@%lu\n",
            tr_kind(r0->key), r0->key & 0x00ffffffUL, r0->first, r0->last,
            r0->count, r0->seq,
            tr_kind(r1->key), r1->key & 0x00ffffffUL, r1->first, r1->last,
            r1->count, r1->seq,
            tr_kind(r2->key), r2->key & 0x00ffffffUL, r2->first, r2->last,
            r2->count, r2->seq);
    }
}

static void d_wr(void *c, uint32_t off, uint32_t v)
{
    volatile unsigned long *csr = iwm_d32_csr();

    (void)c;
    iwm_tr_w(off, v);
    if (off == 0x00cUL) {           /* CSR_INT_MASK: keep the ISR shadow */
        iwm_d32_set_int_mask(v);
        return;
    }
    if (csr)
        csr[off / 4] = v;
}

/* Nothing waits on hardware yet. Replace with udelay() once its prototype is
 * confirmed in Dev32lib.h, before iwm_prepare_card_hw() is ever called. */
static void msi_watch(void);

/* Every wait in the driver is a busy wait in d_delay(): command responses,
 * the firmware loads, the 1.2-4 s UMAC scan behind every XWLAN scan request
 * and the association exchange.  The kernel does not preempt a thread in
 * ring 0, so a long wait in the IOCTL path stalled the whole system.  While
 * an IOCTL is being handled (task time, no spinlock held) the waiting thread
 * gives up the processor once per IWM_YIELD_US of delay.  Not used during
 * INIT or from the interrupt handler. */
#define IWM_YIELD_US 1000UL
static int g_task_time;
static ULONG g_wait_us;          /* delay accumulated since the last yield */
static ULONG g_ioctl_wait_us;    /* delay within the current IOCTL */
static ULONG g_yields;

static void d_delay(void *c, uint32_t us)
{
    (void)c;
    msi_watch();
    udelay((ULONG)us);
    if (!g_task_time)
        return;
    g_ioctl_wait_us += us;
    g_wait_us += us;
    if (g_wait_us >= IWM_YIELD_US) {
        g_wait_us = 0;
        ++g_yields;
        Dev32Help_Yield();
    }
}

static int d_dma_alloc(void *c, uint32_t size, uint32_t alignment,
                       struct iwm_dma_block *block)
{
    ULONG raw_phys, total, shift;
    void *raw;

    (void)c;
    if (!block || size == 0 || alignment == 0 ||
        (alignment & (alignment - 1U)) ||
        size > 0xFFFFFFFFUL - (alignment - 1U))
        return IWM_EINVAL;
    memset(block, 0, sizeof(*block));
    total = (ULONG)size + (ULONG)alignment - 1UL;
    raw = D32AllocMem(total, &raw_phys, 0);
    if (!raw)
        return IWM_ENOMEM;
    shift = (0UL - raw_phys) & ((ULONG)alignment - 1UL);
    block->alloc_vaddr = raw;
    block->alloc_size = total;
    block->vaddr = (uint8_t *)raw + shift;
    block->paddr = raw_phys + shift;
    block->size = size;
    block->alignment = alignment;
    if (iwm_dma32_check(block->paddr, size, alignment) != IWM_OK) {
        D32FreeMem(raw);
        memset(block, 0, sizeof(*block));
        return IWM_ERANGE;
    }
    memset(block->vaddr, 0, size);
    return IWM_OK;
}

static void d_dma_free(void *c, struct iwm_dma_block *block)
{
    void *raw;

    (void)c;
    if (!block)
        return;
    raw = block->alloc_vaddr ? block->alloc_vaddr : block->vaddr;
    if (raw)
        D32FreeMem(raw);
    memset(block, 0, sizeof(*block));
}

static void d_lock(void *c)
{
    (void)c;
    iwm_d32_lock();
}

static void d_unlock(void *c)
{
    (void)c;
    iwm_d32_unlock();
}

static const struct iwm_os_ops g_ops = {
    d_rd, d_wr, d_delay, d_dma_alloc, d_dma_free, d_lock, d_unlock, 0
};

static struct iwm_adapter g_ad;
static struct iwm_scan_cache g_scan;
static struct iwm_blob g_store;
static struct iwm_fw_loader g_loader;
static struct iwm_rx_queue g_rxq;
static struct iwm_tx_transport g_tx;
static struct iwm_ndis_context g_ndis;
static int g_attached;

int iwm_d32_fw_attach(void)
{
    int rc;

    if (g_attached)
        return IWM_EBUSY;
    if (!iwm_d32_csr())
        return IWM_ENODEV;
    iwm_adapter_init(&g_ad, &g_ops, 0);
    iwm_genmac_bind_scan_cache(&g_ad, &g_scan);
    rc = iwm_adapter_probe(&g_ad, IWM_VENDOR, IWM_DEVICE, 0,
                           (uint8_t)iwm_m1_irq);
    if (rc != IWM_OK)
        return rc;
    {
        static uint32_t raw[4];   /* static: see W112 note */
        int mrc = iwm_adapter_read_csr_mac(&g_ad, IWM_9000_MAC_CSR_BASE, raw);
        dprintf(0, "iwm: csr mac rc %d strap %08lx %08lx otp %08lx %08lx "
                "addr %02x:%02x:%02x:%02x:%02x:%02x\n", mrc,
                (ULONG)raw[0], (ULONG)raw[1], (ULONG)raw[2], (ULONG)raw[3],
                g_ad.mac[0], g_ad.mac[1], g_ad.mac[2],
                g_ad.mac[3], g_ad.mac[4], g_ad.mac[5]);
    }
    iwm_rx_init(&g_rxq, &g_ad);
    rc = iwm_rx_alloc(&g_rxq);
    if (rc != IWM_OK) {
        dprintf(0, "iwm: rx alloc rc %d\n", rc);
        return rc;
    }
    dprintf(0, "iwm: rxq free %08lx used %08lx status %08lx buffers %08lx/%lu\n",
            (ULONG)g_rxq.free_desc.paddr, (ULONG)g_rxq.used_desc.paddr,
            (ULONG)g_rxq.status.paddr, (ULONG)g_rxq.buffers.paddr,
            (ULONG)g_rxq.buffers.size);
    iwm_tx_init(&g_tx, &g_ad);
    rc = iwm_tx_alloc(&g_tx);
    if (rc != IWM_OK) {
        dprintf(0, "iwm: tx transport alloc rc %d\n", rc);
        iwm_rx_free(&g_rxq);
        return rc;
    }
    dprintf(0,
        "iwm: tx kw %08lx bc %08lx/%lu tfd %08lx/%lu first %08lx cmd %08lx\n",
        (ULONG)g_tx.keep_warm.paddr, (ULONG)g_tx.scd_bc_table.paddr,
        (ULONG)g_tx.scd_bc_table.size, (ULONG)g_tx.cmd_tfds.paddr,
        (ULONG)g_tx.cmd_tfds.size, (ULONG)g_tx.cmd_first_tb.paddr,
        (ULONG)g_tx.cmd_buffers.paddr);
    iwm_blob_bind(&g_store, &g_store_ops, 0);
    rc = iwm_adapter_set_fw_store(&g_ad, &g_store);
    if (rc == IWM_OK) {
        iwm_fw_loader_init(&g_loader, &g_ad);
        iwm_fw_loader_bind_rx(&g_loader, &g_rxq);
        iwm_fw_loader_bind_tx(&g_loader, &g_tx);
        g_attached = 1;
        iwm_d32_ndis_bind(&g_ndis, &g_ad);
        rc = iwm_ndis_start(&g_ndis);
        dprintf(0, "iwm: NDIS start %d\n", rc);
    } else {
        iwm_tx_free(&g_tx);
        iwm_rx_free(&g_rxq);
    }
    return rc;
}

static void isr_drain(void);

void iwm_d32_irq_notify(unsigned long cause)
{
    if (!g_attached)
        return;
    iwm_fw_loader_note_irq(&g_loader, (uint32_t)cause);
    isr_drain();
}

/* Milestone B: the INIT command sequence (NVM, TX antennas, PHY config,
 * calibration results, MCC), then stop INIT firmware so the runtime image
 * can be loaded next. */
static struct iwm_init_fw g_init;
static struct iwm_rt_fw g_rt;

/* ---- Milestone F: data path.  Received frames are drained from the
 * interrupt handler whenever no IOCTL is running (an IOCTL drains RX itself
 * while it waits), converted to Ethernet II and handed to iwm_ndis_receive().
 * The Drv32 NDIS MAC registered during attach indicates them to the bound
 * protocol; iwm_d32_data_tx() is its transmit entry point. */
static struct iwm_data g_data;
static uint8_t g_rx_eth[IWM_DATA_ETH_MAX];
static int g_in_isr;
static ULONG g_isr_drains;
static int g_deauth_pending;
static uint16_t g_deauth_reason;
static uint8_t g_deauth_fc;

#define IWM_RXLOG 8U
struct iwm_rxlog {
    uint16_t len;
    uint8_t dst[6];
    uint8_t src[6];
    uint8_t b[16];              /* ethertype and first payload bytes */
};
static struct iwm_rxlog g_rxlog[IWM_RXLOG];
static unsigned g_rxlog_n;
static ULONG g_dhcp_replies;    /* UDP to port 68 */
/* d24 probe: two DHCPDISCOVERs with different xid, one asking for a unicast
 * reply (BOOTP flags 0, as Linux dhcpcd does) and one for a broadcast reply.
 * Replies are matched to a probe by xid and by L2 destination. */
#define IWM_DHCP_PROBES 2U
static uint32_t g_probe_xid[IWM_DHCP_PROBES];
static ULONG g_probe_rep[IWM_DHCP_PROBES];      /* replies per probe */
static ULONG g_probe_rep_bc[IWM_DHCP_PROBES];   /* of those, sent to ff:..:ff */
static ULONG g_dhcp_stray;                      /* port 68, unknown xid */
/* Data-queue (4) TX responses.  The d17 log showed only queue-5 responses
 * because the four-entry station log was already full.  Layout read from
 * the logged queue-5 responses (byte_cnt 30/66 at +30, frame control at
 * +34) and Linux struct iwl_mvm_tx_resp from memory: failure_frame +3,
 * initial_rate +4, status le16 +36 (low byte 1 = success). */
static ULONG g_dq_resp;
static ULONG g_dq_ok;
static uint8_t g_dq_last[40];
static uint32_t g_dq_last_len;
static ULONG g_addba_req;         /* ADDBA requests from the BSS */
static ULONG g_addba_ok;          /* declined and queued */
static int g_addba_rc;

/* d27 log: after association the only receive evidence was "rx ok 2" (two
 * IGMP queries) and "notdata 33".  Nothing said whether the AP answered the
 * DHCP requests at all.  These counters cover every 802.11 frame the firmware
 * hands up, by type and subtype, separately for frames addressed to this
 * station; the first few data frames iwm_data_rx() refused keep their header;
 * the first few management frames to or from the BSS (not beacons) keep their
 * header and first body bytes.  Reset at the start of each association. */
static ULONG g_rxt_all[3][16];
static ULONG g_rxt_us[3][16];
#define IWM_REJLOG 8U
struct iwm_rej {
    uint8_t fc0, fc1;
    uint16_t len;
    int rc;
    uint8_t a1[6], a2[6], a3[6];
};
static struct iwm_rej g_rej[IWM_REJLOG];
static unsigned g_rej_n;
static ULONG g_rej_more;
#define IWM_MGMTRX 12U
struct iwm_mgmtrx {
    uint8_t fc0, fc1;
    uint16_t len;
    uint8_t a1[6];
    uint8_t body[8];
};
static struct iwm_mgmtrx g_mgmtrx[IWM_MGMTRX];
static unsigned g_mgmtrx_n;
static ULONG g_mgmtrx_more;

/* w5 log: after association the card logged 707 interrupt-driven RX drains in
 * a minute but only one beacon, and not one data frame, so the per-type
 * counters above (which only see frames that passed iwm_rx_mpdu_frame())
 * cannot say whether the firmware hands up nothing or hands up frames that
 * are thrown away.  These count every notification BEFORE any validation:
 * a histogram by (group, command), the RX MPDUs split into accepted and
 * refused (with the first descriptors of the refused ones), TX responses of
 * queues the driver does not own (frames the firmware sends by itself, e.g.
 * null frames that set the power-management bit), and the traffic indication
 * map of the BSS's own beacons. */
#define IWM_NHIST 16U
struct iwm_nh {
    uint8_t cmd, grp;
    uint16_t maxlen;
    ULONG count;
};
static struct iwm_nh g_nh[IWM_NHIST];
static unsigned g_nh_n;
static ULONG g_nh_lost;
static ULONG g_mpdu_total, g_mpdu_eio, g_mpdu_fmt, g_mpdu_data;
#define IWM_BADLOG 4U
#define IWM_BADBYTES 72U
struct iwm_badmpdu {
    uint16_t len;
    int rc;
    uint8_t b[IWM_BADBYTES];
};
static struct iwm_badmpdu g_bad[IWM_BADLOG];
static unsigned g_bad_n;
#define IWM_TXO 4U
struct iwm_txo {
    uint8_t queue;
    uint16_t fc, status;
    ULONG rate;
};
static struct iwm_txo g_txo[IWM_TXO];
static unsigned g_txo_n;
static ULONG g_txo_count;
static ULONG g_tim_own, g_tim_aid, g_tim_grp;
static uint8_t g_tim_last[4];        /* dtim count, period, control, aid bit */
static ULONG g_rx_pk0, g_rx_nt0;     /* rxq counters at the association start */
static uint16_t g_tim_aidval;        /* AID the TIM bit is tested for */
/* Frames that belong to OUR BSS (management: BSSID field; data: the BSSID of the
 * ToDS/FromDS pair), whoever they are addressed to, by type and subtype.  Other
 * BSSs of the same AP radio add to every other counter; with the promiscuous
 * build these are the only counters that say whether the AP sent anything on
 * this BSS after our DHCPDISCOVERs.  The first IWM_OWNLOG non-null data frames
 * keep their first 64 bytes (header, SNAP, start of the IP header). */
static ULONG g_own[3][16];
static ULONG g_own_us[3][16];       /* of those, receiver address is our MAC */
static ULONG g_mpdu_short;          /* refused: 10..23 byte frame (ACK, CTS ...) */
#define IWM_OWNLOG 6U
#define IWM_OWNBYTES 64U
struct iwm_ownrx {
    uint16_t len;
    uint8_t b[IWM_OWNBYTES];
};
static struct iwm_ownrx g_ownrx[IWM_OWNLOG];
static unsigned g_ownrx_n;
static ULONG g_ownrx_more;

static void rxraw_note(uint8_t cmd, uint8_t group, uint16_t seq,
                       const uint8_t *payload, uint32_t len)
{
    unsigned i;
    for (i = 0; i < g_nh_n; ++i)
        if (g_nh[i].cmd == cmd && g_nh[i].grp == group)
            break;
    if (i == g_nh_n) {
        if (g_nh_n >= IWM_NHIST) {
            ++g_nh_lost;
            i = IWM_NHIST;
        } else {
            g_nh[i].cmd = cmd;
            g_nh[i].grp = group;
            g_nh[i].maxlen = 0;
            g_nh[i].count = 0;
            ++g_nh_n;
        }
    }
    if (i < IWM_NHIST) {
        ++g_nh[i].count;
        if (len > g_nh[i].maxlen)
            g_nh[i].maxlen = (uint16_t)(len > 0xffffUL ? 0xffffUL : len);
    }
    if (group <= 1U && cmd == IWM_CMD_RX_MPDU) {
        const uint8_t *f;
        uint32_t fl;
        int rc = iwm_rx_mpdu_frame(payload, len, &f, &fl);
        ++g_mpdu_total;
        if (rc == IWM_OK) {
            if ((f[0] & 0x0cU) == 0x08U)
                ++g_mpdu_data;
        } else {
            if (rc == IWM_EIO)
                ++g_mpdu_eio;
            else
                ++g_mpdu_fmt;
            if (rc == IWM_EFORMAT && len >= IWM_RX_MPDU_DESC_SIZE + 10U) {
                /* ACK, CTS, PS-Poll ...: shorter than a management header, so
                 * iwm_rx_mpdu_frame() refuses them.  Counted, not dumped. */
                uint32_t ml = iwm_get_le16(payload + IWM_RX_MPDU_LEN_OFF);
                uint16_t sw = iwm_get_le16(payload + IWM_RX_MPDU_STATUS_OFF);
                if (ml >= 10U && ml < IWM_80211_HDR_LEN && (sw & 3U) == 3U) {
                    ++g_mpdu_short;
                    --g_mpdu_fmt;
                    return;
                }
            }
            if (g_bad_n < IWM_BADLOG) {
                struct iwm_badmpdu *b = &g_bad[g_bad_n++];
                b->len = (uint16_t)(len > 0xffffUL ? 0xffffUL : len);
                b->rc = rc;
                memset(b->b, 0, sizeof(b->b));
                memcpy(b->b, payload, len < IWM_BADBYTES ? (unsigned)len
                                                          : IWM_BADBYTES);
            }
        }
    }
    if (group == 0 && cmd == IWM_TX_CMD_TX) {
        unsigned q = (seq >> 8) & 0x1fU;
        if (q != IWM_TX_DATA_QUEUE && q != IWM_TX_MGMT_QUEUE) {
            ++g_txo_count;
            if (g_txo_n < IWM_TXO) {
                struct iwm_txo *t = &g_txo[g_txo_n++];
                t->queue = (uint8_t)q;
                t->fc = len >= 36U ? iwm_get_le16(payload + 34) : 0;
                t->status = len >= 38U ? iwm_get_le16(payload + 36) : 0;
                t->rate = len >= 8U ? iwm_get_le32(payload + 4) : 0;
            }
        }
    }
}

static void rxdiag_reset(void)
{
    memset(g_nh, 0, sizeof(g_nh));
    g_nh_n = 0;
    g_nh_lost = 0;
    g_mpdu_total = g_mpdu_eio = g_mpdu_fmt = g_mpdu_data = 0;
    g_mpdu_short = 0;
    memset(g_own, 0, sizeof(g_own));
    memset(g_own_us, 0, sizeof(g_own_us));
    g_ownrx_n = 0;
    g_ownrx_more = 0;
    g_bad_n = 0;
    g_txo_n = 0;
    g_txo_count = 0;
    g_tim_own = g_tim_aid = g_tim_grp = 0;
    memset(g_tim_last, 0, sizeof(g_tim_last));
    g_rx_pk0 = g_rxq.packets;
    g_rx_nt0 = g_rxq.notifications;
    memset(g_rxt_all, 0, sizeof(g_rxt_all));
    memset(g_rxt_us, 0, sizeof(g_rxt_us));
    g_rej_n = 0;
    g_rej_more = 0;
    g_mgmtrx_n = 0;
    g_mgmtrx_more = 0;
}

/* Called for every 802.11 frame with a valid hardware status. */
static void rxdiag_frame(const uint8_t *f, uint32_t n, const uint8_t *bssid,
                         uint16_t aid, int up)
{
    unsigned t = (f[0] >> 2) & 3U, st = f[0] >> 4;
    int to_us;
    if (t > 2U || n < 24U)
        return;
    to_us = memcmp(f + 4, g_ad.mac, 6) == 0;
    ++g_rxt_all[t][st];
    if (to_us)
        ++g_rxt_us[t][st];
    /* Frames of our own BSS, whoever they are for. */
    {
        const uint8_t *own = 0;
        if (t == 0U)
            own = f + 16;                              /* BSSID */
        else if (t == 2U && (f[1] & 3U) == 2U)
            own = f + 10;                              /* FromDS: A2 is the BSSID */
        else if (t == 2U && (f[1] & 3U) == 1U)
            own = f + 4;                               /* ToDS: A1 is the BSSID */
        if (own && memcmp(own, bssid, 6) == 0) {
            ++g_own[t][st];
            if (to_us)
                ++g_own_us[t][st];
            if (t == 2U && !(f[0] & 0x40U)) {          /* data, not null */
                if (g_ownrx_n < IWM_OWNLOG) {
                    struct iwm_ownrx *o = &g_ownrx[g_ownrx_n++];
                    o->len = (uint16_t)(n > 0xffffUL ? 0xffffUL : n);
                    memset(o->b, 0, sizeof(o->b));
                    memcpy(o->b, f, n < IWM_OWNBYTES ? (unsigned)n : IWM_OWNBYTES);
                } else {
                    ++g_ownrx_more;
                }
            }
        }
    }
    /* Beacon of our own BSS while associated: does the AP hold frames for us? */
    g_tim_aidval = aid;
    if (t == 0U && st == 8U && up && memcmp(f + 16, bssid, 6) == 0) {
        struct iwm_tim_info ti;
        if (iwm_beacon_tim(f, n, aid, &ti) == IWM_OK && ti.present) {
            ++g_tim_own;
            if (ti.aid_set)
                ++g_tim_aid;
            if (ti.group_set)
                ++g_tim_grp;
            g_tim_last[0] = ti.dtim_count;
            g_tim_last[1] = ti.dtim_period;
            g_tim_last[2] = ti.bitmap_ctl;
            g_tim_last[3] = ti.aid_set;
        }
    }
    /* Management frames of the BSS, except beacons and probe responses. */
    if (t == 0U && st != 8U && st != 5U &&
        (to_us || memcmp(f + 10, bssid, 6) == 0)) {
        if (g_mgmtrx_n < IWM_MGMTRX) {
            struct iwm_mgmtrx *m = &g_mgmtrx[g_mgmtrx_n++];
            m->fc0 = f[0];
            m->fc1 = f[1];
            m->len = (uint16_t)(n > 0xffffUL ? 0xffffUL : n);
            memcpy(m->a1, f + 4, 6);
            memset(m->body, 0, sizeof(m->body));
            memcpy(m->body, f + 24, n - 24U < 8U ? (unsigned)(n - 24U) : 8U);
        } else {
            ++g_mgmtrx_more;
        }
    }
}

/* A data-type frame iwm_data_rx() refused: remember its header. */
static void rxdiag_reject(const uint8_t *payload, uint32_t len, int rc)
{
    const uint8_t *f;
    uint32_t fl;
    struct iwm_rej *r;
    if (iwm_rx_mpdu_frame(payload, len, &f, &fl) != IWM_OK ||
        (f[0] & 0x0cU) != 0x08U)
        return;
    if (g_rej_n >= IWM_REJLOG) {
        ++g_rej_more;
        return;
    }
    r = &g_rej[g_rej_n++];
    r->fc0 = f[0];
    r->fc1 = f[1];
    r->len = (uint16_t)(fl > 0xffffUL ? 0xffffUL : fl);
    r->rc = rc;
    memcpy(r->a1, f + 4, 6);
    memcpy(r->a2, f + 10, 6);
    memcpy(r->a3, f + 16, 6);
}

static int why_is(const char *why, const char *want)
{
    while (*want && *why == *want) {
        ++why;
        ++want;
    }
    return *want == 0 && *why == 0;
}

static void log_frame(const char *tag, const uint8_t *f, unsigned n);

static void log_rxdiag(const char *why)
{
    static const char *const tname[3] = { "mgmt", "ctrl", "data" };
    unsigned t, s, i;
    int detail = !why_is(why, "periodic");
    dprintf(0,
        "iwm: rxraw %s rxq pkts %lu notif %lu malformed %lu mpdu %lu data-ok %lu bad-status %lu bad-format %lu hist-lost %lu\n",
        why, (ULONG)(g_rxq.packets - g_rx_pk0),
        (ULONG)(g_rxq.notifications - g_rx_nt0), (ULONG)g_rxq.malformed,
        g_mpdu_total, g_mpdu_data, g_mpdu_eio, g_mpdu_fmt, g_nh_lost);
    dprintf(0, "iwm: rxshort %s control frames shorter than a header %lu\n",
            why, g_mpdu_short);
    for (t = 0; t < 3U; ++t)
        for (s = 0; s < 16U; ++s)
            if (g_own[t][s])
                dprintf(0, "iwm: ownbss %s %s subtype %u count %lu to-us %lu\n",
                        why, tname[t], s, g_own[t][s], g_own_us[t][s]);
    dprintf(0,
        "iwm: tim %s own-beacons %lu aid %u set %lu group-set %lu last dtim %u/%u ctl %02x aidbit %u txresp-other %lu\n",
        why, g_tim_own, (unsigned)g_tim_aidval, g_tim_aid, g_tim_grp,
        (unsigned)g_tim_last[0], (unsigned)g_tim_last[1],
        (unsigned)g_tim_last[2], (unsigned)g_tim_last[3], g_txo_count);
    for (t = 0; t < 3U; ++t)
        for (s = 0; s < 16U; ++s)
            if (g_rxt_all[t][s])
                dprintf(0, "iwm: rxtype %s %s %s subtype %u all %lu to-us %lu\n",
                        why, tname[t], t == 2U ? "data" : "frame", s,
                        g_rxt_all[t][s], g_rxt_us[t][s]);
    if (!detail)
        return;
    for (i = 0; i < g_ownrx_n; ++i) {
        dprintf(0, "iwm: ownbss data %u len %u\n", i, (unsigned)g_ownrx[i].len);
        log_frame("ownbss", g_ownrx[i].b, IWM_OWNBYTES);
    }
    if (g_ownrx_more)
        dprintf(0, "iwm: ownbss data more %lu\n", g_ownrx_more);
    for (i = 0; i < g_nh_n; ++i)
        dprintf(0, "iwm: rxhist grp %02x cmd %02x count %lu maxlen %u\n",
                (unsigned)g_nh[i].grp, (unsigned)g_nh[i].cmd, g_nh[i].count,
                (unsigned)g_nh[i].maxlen);
    for (i = 0; i < g_txo_n; ++i)
        dprintf(0, "iwm: txresp-other %u queue %u fc %04x status %04x rate %08lx\n",
                i, (unsigned)g_txo[i].queue, (unsigned)g_txo[i].fc,
                (unsigned)g_txo[i].status, (ULONG)g_txo[i].rate);
    for (i = 0; i < g_bad_n; ++i) {
        dprintf(0, "iwm: badmpdu %u len %u rc %d\n", i,
                (unsigned)g_bad[i].len, g_bad[i].rc);
        log_frame("badmpdu", g_bad[i].b, IWM_BADBYTES);
    }
    for (i = 0; i < g_rej_n; ++i) {
        const struct iwm_rej *r = &g_rej[i];
        dprintf(0,
            "iwm: datarej %u fc %02x%02x len %u rc %d a1 %02x:%02x:%02x:%02x:%02x:%02x a2 %02x:%02x:%02x:%02x:%02x:%02x a3 %02x:%02x:%02x:%02x:%02x:%02x\n",
            i, (unsigned)r->fc0, (unsigned)r->fc1, (unsigned)r->len, r->rc,
            r->a1[0], r->a1[1], r->a1[2], r->a1[3], r->a1[4], r->a1[5],
            r->a2[0], r->a2[1], r->a2[2], r->a2[3], r->a2[4], r->a2[5],
            r->a3[0], r->a3[1], r->a3[2], r->a3[3], r->a3[4], r->a3[5]);
    }
    if (g_rej_more)
        dprintf(0, "iwm: datarej more %lu\n", g_rej_more);
    for (i = 0; i < g_mgmtrx_n; ++i) {
        const struct iwm_mgmtrx *m = &g_mgmtrx[i];
        dprintf(0,
            "iwm: mgmtrx %u fc %02x%02x len %u da %02x:%02x:%02x:%02x:%02x:%02x body %02x%02x%02x%02x %02x%02x%02x%02x\n",
            i, (unsigned)m->fc0, (unsigned)m->fc1, (unsigned)m->len,
            m->a1[0], m->a1[1], m->a1[2], m->a1[3], m->a1[4], m->a1[5],
            m->body[0], m->body[1], m->body[2], m->body[3], m->body[4],
            m->body[5], m->body[6], m->body[7]);
    }
    if (g_mgmtrx_more)
        dprintf(0, "iwm: mgmtrx more %lu\n", g_mgmtrx_more);
}

static void data_rx(const uint8_t *payload, uint32_t len)
{
    uint16_t n = 0;
    int drc = iwm_data_rx(&g_data, payload, len, g_rx_eth, sizeof(g_rx_eth), &n);
    if (drc != IWM_OK) {
        rxdiag_reject(payload, len, drc);
        return;
    }
    if (n >= 50U && g_rx_eth[12] == 0x08 && g_rx_eth[13] == 0x00 &&
        g_rx_eth[23] == 17 && g_rx_eth[36] == 0 && g_rx_eth[37] == 68) {
        /* BOOTP xid: Ethernet 14 + IP 20 + UDP 8 + op/htype/hlen/hops 4. */
        uint32_t xid = ((uint32_t)g_rx_eth[46] << 24) |
                       ((uint32_t)g_rx_eth[47] << 16) |
                       ((uint32_t)g_rx_eth[48] << 8) | g_rx_eth[49];
        unsigned k;
        ++g_dhcp_replies;
        for (k = 0; k < IWM_DHCP_PROBES && xid != g_probe_xid[k]; ++k)
            ;
        if (k < IWM_DHCP_PROBES) {
            ++g_probe_rep[k];
            if ((g_rx_eth[0] & g_rx_eth[1] & g_rx_eth[2] & g_rx_eth[3] &
                 g_rx_eth[4] & g_rx_eth[5]) == 0xffU)
                ++g_probe_rep_bc[k];
        } else {
            ++g_dhcp_stray;
        }
    }
    if (g_rxlog_n < IWM_RXLOG) {
        struct iwm_rxlog *l = &g_rxlog[g_rxlog_n++];
        l->len = n;
        memcpy(l->dst, g_rx_eth, 6);
        memcpy(l->src, g_rx_eth + 6, 6);
        memcpy(l->b, g_rx_eth + 12, n >= 28U ? 16U : (unsigned)(n - 12U));
    }
    iwm_ndis_receive(&g_ndis, g_rx_eth, n);
}

/* Notification hook: station layer (time event, TX responses) and data. */
static void os2_notif(void *ctx, uint8_t cmd, uint8_t group, uint16_t seq,
                      const uint8_t *payload, uint32_t len)
{
    rxraw_note(cmd, group, seq, payload, len);
    iwm_sta_notif(ctx, cmd, group, seq, payload, len);
    if (group == 0 && cmd == IWM_TX_CMD_TX &&
        ((seq >> 8) & 0x1fU) == IWM_TX_DATA_QUEUE) {
        ++g_dq_resp;
        g_dq_last_len = len;
        memset(g_dq_last, 0, sizeof(g_dq_last));
        memcpy(g_dq_last, payload, len < sizeof(g_dq_last) ? len : sizeof(g_dq_last));
        if (len >= 38U && g_dq_last[36] == 0x01)
            ++g_dq_ok;
    }
    if (group <= 1U && cmd == IWM_CMD_RX_MPDU && g_data.up)
        data_rx(payload, len);
}

int iwm_d32_data_tx(const void *eth, unsigned short len)
{
    return iwm_data_tx(&g_data, (const uint8_t *)eth, (uint16_t)len);
}

/* From the interrupt handler: drain RX when the runtime firmware is up and
 * no IOCTL owns the queue.  g_task_time changes only under the same lock. */
static void isr_drain(void)
{
    if (g_ad.firmware.state != IWM_FW_RUNNING || !g_rxq.programmed)
        return;
    iwm_d32_lock();
    if (!g_task_time) {
        g_in_isr = 1;
        ++g_isr_drains;
        (void)iwm_rx_drain(&g_rxq);
        g_in_isr = 0;
    }
    iwm_d32_unlock();
}

static void log_data(const char *why)
{
    unsigned i;
    dprintf(0,
        "iwm: data %s up %u tx ok %lu err %lu rc %ld rx ok %lu (ucast %lu bcast %lu mcast %lu) dhcp %lu notdata %lu otherbss %lu echo %lu prot %lu amsdu %lu null %lu bad %lu isr drains %lu\n",
        why, (unsigned)g_data.up, (ULONG)g_data.tx_ok, (ULONG)g_data.tx_err,
        (long)g_data.tx_last_rc, (ULONG)g_data.rx_ok, (ULONG)g_data.rx_ucast,
        (ULONG)g_data.rx_bcast, (ULONG)g_data.rx_mcast, g_dhcp_replies,
        (ULONG)g_data.rx_not_data, (ULONG)g_data.rx_other_bss,
        (ULONG)g_data.rx_echo, (ULONG)g_data.rx_protected,
        (ULONG)g_data.rx_amsdu, (ULONG)g_data.rx_null, (ULONG)g_data.rx_bad,
        g_isr_drains);
    dprintf(0,
        "iwm: dataq resp %lu ok %lu len %lu fail %u rate %08lx bytes %u fc %04x status %04x\n",
        g_dq_resp, g_dq_ok, (ULONG)g_dq_last_len, (unsigned)g_dq_last[3],
        (ULONG)iwm_get_le32(g_dq_last + 4), (unsigned)iwm_get_le16(g_dq_last + 30),
        (unsigned)iwm_get_le16(g_dq_last + 34), (unsigned)iwm_get_le16(g_dq_last + 36));
    dprintf(0, "iwm: addba req %lu declined %lu rc %d\n", g_addba_req,
            g_addba_ok, g_addba_rc);
    dprintf(0, "iwm: txq stale data %lu mgmt %lu nested rx drains %lu\n",
            (ULONG)iwm_tx_fq_get(&g_tx, IWM_TX_DATA_QUEUE)->stale,
            (ULONG)iwm_tx_fq_get(&g_tx, IWM_TX_MGMT_QUEUE)->stale,
            (ULONG)g_rxq.nested_drains);
    log_rxdiag(why);
    iwm_d32_ndis_log(why);
    iwm_d32_ndis_log_rx(why);
    for (i = 0; i < g_rxlog_n; ++i) {
        const struct iwm_rxlog *l = &g_rxlog[i];
        dprintf(0,
            "iwm: datarx %u len %u dst %02x:%02x:%02x:%02x:%02x:%02x src %02x:%02x:%02x:%02x:%02x:%02x %02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x\n",
            i, (unsigned)l->len, l->dst[0], l->dst[1], l->dst[2], l->dst[3],
            l->dst[4], l->dst[5], l->src[0], l->src[1], l->src[2], l->src[3],
            l->src[4], l->src[5], l->b[0], l->b[1], l->b[2], l->b[3], l->b[4],
            l->b[5], l->b[6], l->b[7], l->b[8], l->b[9], l->b[10], l->b[11],
            l->b[12], l->b[13], l->b[14], l->b[15]);
    }
    g_rxlog_n = 0;
}

/* d20: the first disconnect left the firmware silent.  Teardown command 45
 * timed out, so did every command after it, and no interrupt arrived again
 * (isr 351 / msi 348 unchanged), but the log held nothing to say why.  When
 * the command-timeout count has grown since the last call, log what Linux's
 * iwl_mvm_dump_nic_error_log() reads: the LMAC and UMAC error tables (LMAC
 * dword 0 is "valid", dword 1 the error id; UMAC dword 0 is the error id),
 * the interrupt registers and the CPU program counters. */
static ULONG g_health_seen;

static ULONG health_prph(ULONG reg)
{
    d_wr(0, 0x448UL, (uint32_t)(reg | 0x03000000UL));    /* PRPH_RADDR */
    return (ULONG)d_rd(0, 0x450UL);                      /* PRPH_RDAT */
}

static void health_mem(ULONG addr, ULONG *out, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; ++i)
        out[i] = 0;
    if (!addr)
        return;
    d_wr(0, 0x40cUL, (uint32_t)addr);                    /* MEM_RADDR */
    for (i = 0; i < n; ++i)
        out[i] = (ULONG)d_rd(0, 0x41cUL);                /* MEM_RDAT */
}

static void fw_health(void)
{
    static ULONG lm[8], um[8];   /* static: see W112 note */
    ULONG sb1 = 0, sb2 = 0, upc = 0, lpc = 0;
    int nrc;
    if (g_rt.cmd_timeouts == g_health_seen)
        return;
    g_health_seen = g_rt.cmd_timeouts;
    nrc = iwm_grab_nic_access(&g_ad);
    if (nrc == IWM_OK) {
        sb1 = health_prph(0x00a01e30UL);
        sb2 = health_prph(0x00a01e34UL);
        upc = health_prph(0x00a05c18UL);
        lpc = health_prph(0x00a05c1cUL);
        health_mem(g_ad.firmware.catalog.lmac_err_addr, lm, 8U);
        health_mem(g_ad.firmware.catalog.umac_err_addr, um, 8U);
        iwm_release_nic_access(&g_ad);
    }
    dprintf(0,
        "iwm: fwhealth timeouts %u in-a-row %u dead %u failcmd %02x seq %04x nic %d int %08lx mask %08lx fh %08lx gp %08lx reset %08lx\n",
        (unsigned)g_rt.cmd_timeouts, (unsigned)g_rt.consec_timeouts,
        (unsigned)g_rt.dead, (unsigned)g_rt.fail_cmd, (unsigned)g_rt.fail_seq,
        nrc, (ULONG)d_rd(0, 0x008UL), (ULONG)d_rd(0, 0x00cUL),
        (ULONG)d_rd(0, 0x010UL), (ULONG)d_rd(0, 0x024UL),
        (ULONG)d_rd(0, 0x020UL));
    dprintf(0,
        "iwm: fwhealth sb1 %08lx sb2 %08lx umacpc %08lx lmacpc %08lx tx wr %u rd %u rx read %u closed %u pkts %lu\n",
        sb1, sb2, upc, lpc, (unsigned)g_tx.write, (unsigned)g_tx.read,
        (unsigned)g_rxq.read, (unsigned)g_rxq.last_closed,
        (ULONG)g_rxq.packets);
    dprintf(0,
        "iwm: fwerr lmac @%08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n",
        (ULONG)g_ad.firmware.catalog.lmac_err_addr, lm[0], lm[1], lm[2],
        lm[3], lm[4], lm[5], lm[6], lm[7]);
    dprintf(0,
        "iwm: fwerr umac @%08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n",
        (ULONG)g_ad.firmware.catalog.umac_err_addr, um[0], um[1], um[2],
        um[3], um[4], um[5], um[6], um[7]);
}

/* d5 log: MSI Enable went 1 -> 0 between "intdump initload" (isr 7) and
 * "intdump initseq" (isr 11) and stayed set for 120+ interrupts after the
 * re-arm, so the clear is tied to something in the INIT command phase.
 * While g_msi_watch is set, every task-level delay (all command and RX
 * waits poll through d_delay) samples the control word once and logs the
 * first observation of the clear with the command/RX position. */
static int g_msi_watch;
static int g_msi_off_seen;

static void msi_watch(void)
{
    unsigned ctl;
    if (!g_msi_watch || g_msi_off_seen)
        return;
    ctl = iwm_d32_msi_ctl();
    if (ctl & 0x0001U)
        return;
    g_msi_off_seen = 1;
    dprintf(0,
        "iwm: msi off seen ctl %04x isr-msi %lu stage %u nvm %lu phydb %u notif %lu rx pkts %lu last cmd %02x grp %02x seq %04x tx wr %u rd %u\n",
        ctl, (ULONG)iwm_m1_irq_count, (unsigned)g_init.stage,
        (ULONG)g_init.nvm_used, (unsigned)g_init.phydb_count,
        (ULONG)g_init.notif_count, (ULONG)g_rxq.packets,
        (unsigned)g_rxq.last_cmd, (unsigned)g_rxq.last_group,
        (unsigned)g_rxq.last_seq, (unsigned)g_tx.write, (unsigned)g_tx.read);
}

/* One line per replayed Linux command from cmd_log[first..]. */
static void log_rt_cmds(unsigned first)
{
    unsigned i;
    for (i = first; i < g_rt.cmd_count && i < IWM_RT_CMD_LOG; ++i)
        dprintf(0, "iwm: rtcmd %u id %03x len %u rc %d resp %u %08lx %08lx\n",
                i, (unsigned)g_rt.cmd_log[i].id, (unsigned)g_rt.cmd_log[i].len,
                (int)g_rt.cmd_log[i].rc, (unsigned)g_rt.cmd_log[i].resp_len,
                (ULONG)g_rt.cmd_log[i].resp0, (ULONG)g_rt.cmd_log[i].resp1);
}

/* Milestone D, first step: the remaining Linux runtime commands and one UMAC
 * scan.  Logs every command result, the notification histogram, the scan
 * complete payload and the first RX MPDU frames for the result parser.  The
 * firmware session stays LOADING, so XWLAN scan requests remain gated. */
/* Scan-cache contents and per-scan parse counters. */
static void log_scan_cache(const char *why, int rc)
{
    static char ssid[33];   /* static: see W112 note */
    unsigned i, k;
    dprintf(0,
        "iwm: scan %s rc %d frames %lu parsed %lu skipped %lu bad %lu lost %lu wait %lu us cache %u\n",
        why, rc, (ULONG)g_rt.scan_frames, (ULONG)g_rt.scan_parsed,
        (ULONG)g_rt.scan_skipped, (ULONG)g_rt.scan_bad,
        (ULONG)g_rt.scan_lost, (ULONG)g_rt.scan_wait_us,
        (unsigned)g_scan.count);
    for (i = 0; i < g_scan.count; ++i) {
        const struct iwm_scan_result *s = &g_scan.result[i];
        for (k = 0; k < s->ssid_length && k < 32U; ++k)
            ssid[k] = (s->ssid[k] < 0x20 || s->ssid[k] > 0x7e) ?
                      '?' : (char)s->ssid[k];
        ssid[k] = 0;
        dprintf(0,
            "iwm: bss %02x:%02x:%02x:%02x:%02x:%02x khz %lu rssi %ld priv %u mode %u ies %u ssid '%s'\n",
            s->bssid[0], s->bssid[1], s->bssid[2], s->bssid[3],
            s->bssid[4], s->bssid[5], (ULONG)s->ds_config_khz,
            (long)s->rssi_dbm, (unsigned)s->privacy,
            (unsigned)s->infrastructure_mode, (unsigned)s->ie_length, ssid);
    }
}

/* XWLAN OID_802_11_BSSID_LIST_SCAN: GenMAC has emptied the cache and set
 * SCANNING; run one firmware scan synchronously, then complete it so the
 * following OID_802_11_BSSID_LIST query returns the results. */
static int g_scan_busy;

static void run_xwlan_scan(void)
{
    int rc;
    if (g_scan_busy) {
        dprintf(0, "iwm: scan request while a scan is running, ignored\n");
        return;
    }
    if (g_ad.connected) {
        /* Associated: answer from the cache; GenMAC kept it and the
         * ASSOCIATED state.  No off-channel firmware scan. */
        iwm_genmac_scan_complete(&g_ad, IWM_OK);
        dprintf(0, "iwm: scan xwlan answered from cache %u entries\n",
                (unsigned)g_scan.count);
        log_data("periodic");
        return;
    }
    g_scan_busy = 1;
    (void)iwm_d32_msi_rearm("xwlan");
    rc = iwm_rt_fw_scan(&g_rt);
    iwm_genmac_scan_complete(&g_ad, rc);
    log_scan_cache("xwlan", rc);
    iwm_d32_int_dump("xwlan");
    g_scan_busy = 0;
}

/* ---- Milestones E1/E2: association.  E1 (core/iwm_assoc.c) runs the 802.11
 * exchange; E2 (core/iwm_sta.c) replays the captured Linux firmware commands
 * and sends the frames on queue 5.  Only 2.4 GHz was captured, so selection
 * is limited to it. */
#define IWM_AS_MAX_WAIT_US 4000000UL
#define IWM_AS_MAX_KHZ     3000000UL
/* d18 log: each scan reports only 2-4 BSSs and replaces the cache; the
 * 2.4 GHz "fradypondfarms Guest" entry appeared in 4 of 19 scans and was
 * missing from the cache both times XWLAN asked to connect.  On a miss,
 * scan again (results are added to the cache, not replacing it) before
 * giving up. */
#define IWM_AS_SCAN_RETRIES 4U

/* Build-time policy, set on the command line:
 *   wmake -f makefile.wat driver IWM_DEFS=-dIWM_HT=1
 * IWM_HT=1 announces HT (20 MHz, no aggregation) in the association request
 * when the AP has an HT element.  Off by default: it changes what the AP
 * sends, so enable it only after the legacy data path works.  A-MPDU
 * aggregation is declined (see as_mgmt_rx), so no reorder buffer is needed. */
#ifndef IWM_HT
#define IWM_HT 0
#endif
/* Experiment, off by default: wmake -f makefile.wat driver IWM_DEFS=-dIWM_RXPROMISC=1
 * ORs the promiscuous and control/management filter bits into filter_flags of
 * every replayed MAC context (struct iwm_sta.filter_or), so the firmware hands
 * up frames of other stations too.  The command sequence itself is unchanged
 * (w6-c: an extra MAC context after association broke beacon tracking and data
 * TX).  Judge a run by the "ownbss" lines, not by the totals: other BSSs of
 * the same AP fill the totals. */
#ifndef IWM_RXPROMISC
#define IWM_RXPROMISC 0
#endif
static struct iwm_assoc g_as;
static struct iwm_sta g_sta;
static struct iwm_scan_result g_as_bss;
static ULONG g_as_ms;
/* d20: association succeeded but the AP never answered a data frame.  What the
 * AP agreed to (WMM parameter element, HT, rates) is in its association
 * response, which was never logged. */
static uint8_t g_as_resp[320];   /* w3 log: the AP's response is 227 bytes */
static unsigned g_as_resp_len;

static void log_frame(const char *tag, const uint8_t *f, unsigned n)
{
    static char line[33];   /* static: see W112 note */
    static const char hex[] = "0123456789abcdef";
    unsigned off, k, w;
    for (off = 0; off < n; off += 16U) {
        w = n - off < 16U ? n - off : 16U;
        for (k = 0; k < w; ++k) {
            line[2U * k] = hex[f[off + k] >> 4];
            line[2U * k + 1U] = hex[f[off + k] & 15U];
        }
        line[2U * w] = 0;
        dprintf(0, "iwm: %s +%02x %s\n", tag, off, line);
    }
}

/* Every firmware command the station layer sent since index first. */
static void log_sta_cmds(unsigned first)
{
    unsigned i;
    for (i = first; i < g_sta.log_n; ++i)
        dprintf(0, "iwm: stacmd %u id %03x rc %d resp %u %08lx %08lx %08lx\n",
                i, (unsigned)g_sta.log[i].id, (int)g_sta.log[i].rc,
                (unsigned)g_sta.log[i].resp_len, (ULONG)g_sta.log[i].resp0,
                (ULONG)g_sta.log[i].resp1, (ULONG)g_sta.log[i].resp2);
}

static int as_prepare(void *c, const struct iwm_scan_result *bss)
{
    int rc;
    (void)c;
    rc = iwm_sta_prepare(&g_sta, bss, g_ad.mac);
    log_sta_cmds(0);
    dprintf(0,
        "iwm: sta prepare rc %d phase %u ch %u bi %u te id %08lx started %u wait %lu us notif %u %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
        rc, (unsigned)g_sta.phase, (unsigned)g_sta.channel,
        (unsigned)g_sta.beacon_int, (ULONG)g_sta.te_id,
        (unsigned)g_sta.te_started, (ULONG)g_sta.te_wait_us,
        (unsigned)g_sta.te_notif_len,
        g_sta.te_notif[0], g_sta.te_notif[1], g_sta.te_notif[2], g_sta.te_notif[3],
        g_sta.te_notif[4], g_sta.te_notif[5], g_sta.te_notif[6], g_sta.te_notif[7],
        g_sta.te_notif[8], g_sta.te_notif[9], g_sta.te_notif[10], g_sta.te_notif[11],
        g_sta.te_notif[12], g_sta.te_notif[13], g_sta.te_notif[14], g_sta.te_notif[15],
        g_sta.te_notif[16], g_sta.te_notif[17], g_sta.te_notif[18], g_sta.te_notif[19]);
    return rc;
}

static int as_tx_mgmt(void *c, const uint8_t *f, uint16_t n)
{
    unsigned first = g_sta.log_n;
    int rc;
    (void)c;
    rc = iwm_sta_tx_mgmt(&g_sta, f, n);
    log_sta_cmds(first);
    dprintf(0, "iwm: assoc tx fc %02x len %u rc %d phase %u sent %lu resp %lu\n",
            (unsigned)f[0], (unsigned)n, rc, (unsigned)g_sta.phase,
            (ULONG)g_sta.tx_sent, (ULONG)g_sta.tx_resp);
    log_frame("txfrm", f, n);
    return rc;
}

static void as_up(void *c, uint16_t aid)
{
    unsigned first = g_sta.log_n;
    int rc;
    (void)c;
    rc = iwm_sta_up(&g_sta, aid);
    log_sta_cmds(first);
    dprintf(0, "iwm: assoc up aid %u sta rc %d phase %u\n", (unsigned)aid, rc,
            (unsigned)g_sta.phase);
    if (rc == IWM_OK) {
        iwm_data_link(&g_data, g_ad.mac, g_as_bss.bssid);
        iwm_genmac_link_up(&g_ad, g_as_bss.bssid, g_as_bss.rssi_dbm);
    }
}

static void as_down(void *c)
{
    unsigned first = g_sta.log_n;
    (void)c;
    iwm_data_unlink(&g_data);
    iwm_sta_teardown(&g_sta);
    log_sta_cmds(first);
    iwm_genmac_link_down(&g_ad);
    dprintf(0, "iwm: assoc down\n");
}

static const struct iwm_assoc_ops g_as_ops = {
    as_prepare, as_tx_mgmt, as_up, as_down
};

/* Raw TX responses of the frames sent (layout not yet decoded). */
static void log_txresp(void)
{
    unsigned i, j;
    for (i = 0; i < g_sta.txresp_n; ++i)
        for (j = 0; j < IWM_STA_TXRESP_BYTES; j += 16U) {
            const uint8_t *b = &g_sta.txresp[i][j];
            dprintf(0,
                "iwm: txresp %u seq %04x len %u +%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
                i, (unsigned)g_sta.txresp_seq[i], (unsigned)g_sta.txresp_len[i],
                j, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9],
                b[10], b[11], b[12], b[13], b[14], b[15]);
        }
}

/* Every received 802.11 frame, from the RX notification path. */
static void as_mgmt_rx(void *c, const uint8_t *f, uint32_t n)
{
    static const uint8_t bc[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    (void)c;
    if (g_as.state != IWM_AS_IDLE)
        rxdiag_frame(f, n, g_as_bss.bssid, g_as.aid, g_as.state == IWM_AS_UP);
    if (g_as.state == IWM_AS_AUTH || g_as.state == IWM_AS_ASSOC) {
        if (f[0] == IWM_FC0_ASSOC_RESP && n >= 30U && g_as_resp_len == 0 &&
            memcmp(f + 16, g_as_bss.bssid, 6) == 0) {
            g_as_resp_len = n < sizeof(g_as_resp) ? (unsigned)n
                                                  : (unsigned)sizeof(g_as_resp);
            memcpy(g_as_resp, f, g_as_resp_len);
        }
        iwm_assoc_rx(&g_as, f, (uint16_t)(n > 0xffffUL ? 0xffffUL : n),
                     (uint32_t)g_as_ms);
        return;
    }
    /* ADDBA request from the AP: decline it.  Only a frame is queued on
     * management queue 5 (no firmware command, nothing to wait for), so this
     * is safe from the RX callback, including inside an inner drain. */
    if (g_as.state == IWM_AS_UP && n >= 33U && f[0] == IWM_FC0_ACTION) {
        static uint8_t addba[48];   /* static: see W112 note */
        uint16_t l = iwm_assoc_build_addba_decline(&g_as, f,
                         (uint16_t)(n > 0xffffUL ? 0xffffUL : n), addba,
                         sizeof(addba));
        if (l) {
            ++g_addba_req;
            g_addba_rc = iwm_sta_tx_mgmt(&g_sta, addba, l);
            if (g_addba_rc == IWM_OK)
                ++g_addba_ok;
        }
    }
    /* While associated, a deauthentication or disassociation from the AP is
     * only recorded here: the teardown sends firmware commands and waits,
     * which must not run inside an RX drain (interrupt or IOCTL).  It is
     * handled at the next IOCTL by handle_deauth(). */
    if (g_as.state == IWM_AS_UP && n >= 26U &&
        (f[0] == IWM_FC0_DEAUTH || f[0] == IWM_FC0_DISASSOC) &&
        memcmp(f + 10, g_as_bss.bssid, 6) == 0 &&
        (memcmp(f + 4, g_ad.mac, 6) == 0 || memcmp(f + 4, bc, 6) == 0)) {
        g_deauth_fc = f[0];
        g_deauth_reason = (uint16_t)(f[24] | (f[25] << 8));
        g_deauth_pending = 1;
    }
}

static void handle_deauth(void)
{
    unsigned first = g_sta.log_n;
    if (!g_deauth_pending)
        return;
    g_deauth_pending = 0;
    if (g_as.state != IWM_AS_UP)
        return;
    dprintf(0, "iwm: assoc lost: AP sent %s reason %u\n",
            g_deauth_fc == IWM_FC0_DEAUTH ? "deauth" : "disassoc",
            (unsigned)g_deauth_reason);
    g_as.state = IWM_AS_FAILED;
    g_as.fail = IWM_AF_DEAUTH;
    g_as.status = g_deauth_reason;
    iwm_data_unlink(&g_data);
    iwm_sta_teardown(&g_sta);
    log_sta_cmds(first);
    iwm_genmac_link_down(&g_ad);
    log_data("lost");
}

/* Link test after association: one broadcast DHCPDISCOVER on queue 4, then
 * up to 3 s of RX.  A DHCPOFFER (UDP to port 68) proves both directions. */
/* One DHCPDISCOVER and up to 3 s of RX.  Probe 0 asks for a unicast reply,
 * probe 1 for a broadcast reply (the d24 build sent only the second kind). */
static void dhcp_probe_one(unsigned k, uint16_t flags)
{
    static uint8_t frame[IWM_DATA_DHCP_LEN];
    ULONG us;
    uint16_t n;
    int rc;
    g_probe_xid[k] = 0x49570000UL | ((ULONG)k << 8) | (g_as.aid & 0xffU);
    g_probe_rep[k] = 0;
    g_probe_rep_bc[k] = 0;
    n = iwm_data_dhcp_discover_full(g_ad.mac, g_probe_xid[k], flags, frame,
                                    sizeof(frame));
    rc = iwm_data_tx(&g_data, frame, n);
    dprintf(0, "iwm: dhcp probe %u flags %04x xid %08lx tx rc %d len %u\n", k,
            (unsigned)flags, (ULONG)g_probe_xid[k], rc, (unsigned)n);
    for (us = 0; rc == IWM_OK && !g_probe_rep[k] && us < 3000000UL;
         us += 100UL) {
        (void)iwm_rx_drain(&g_rxq);
        d_delay(0, 100U);
    }
    dprintf(0, "iwm: dhcp probe %u waited %lu ms replies %lu (to broadcast %lu)\n",
            k, us / 1000UL, g_probe_rep[k], g_probe_rep_bc[k]);
}

static void run_dhcp_probe(void)
{
    g_rxlog_n = 0;
    g_dhcp_replies = 0;
    g_dhcp_stray = 0;
    g_sta.txresp_n = 0;     /* the four slots were full of auth/assoc */
    dhcp_probe_one(0, 0);
    dhcp_probe_one(1, IWM_DHCP_BROADCAST);
    dprintf(0, "iwm: dhcp probe result unicast-reply %lu broadcast-reply %lu stray %lu\n",
            g_probe_rep[0], g_probe_rep[1], g_dhcp_stray);
    log_txresp();
    log_data("probe");
}

/* OID_802_11_SSID SET (or DISASSOCIATE) recorded an association request. */
static void run_associate(void)
{
    static char name[33];   /* static: see W112 note */
    ULONG us;
    unsigned k;
    int rc;
    if (g_ad.assoc_request == 2) {
        g_ad.assoc_request = 0;
        iwm_assoc_stop(&g_as);           /* deauthentication, then down() */
        iwm_sta_teardown(&g_sta);        /* not up: short teardown */
        iwm_genmac_link_down(&g_ad);
        dprintf(0, "iwm: assoc stopped\n");
        return;
    }
    g_ad.assoc_request = 0;
    if (g_as.state == IWM_AS_UP) {       /* new SSID while associated */
        iwm_assoc_stop(&g_as);
        dprintf(0, "iwm: assoc left previous BSS\n");
    }
    iwm_sta_teardown(&g_sta);
    for (k = 0; k < g_ad.ssid_length && k < 32U; ++k)
        name[k] = (g_ad.ssid[k] < 0x20 || g_ad.ssid[k] > 0x7e) ?
                  '?' : (char)g_ad.ssid[k];
    name[k] = 0;
    rc = iwm_assoc_select_band(&g_scan, g_ad.ssid, g_ad.ssid_length,
                               g_ad.req_bssid_valid ? g_ad.req_bssid : 0,
                               IWM_AS_MAX_KHZ, &g_as_bss);
    for (k = 0; rc != IWM_OK && k < IWM_AS_SCAN_RETRIES && !g_scan_busy; ++k) {
        int src;
        g_scan_busy = 1;
        src = iwm_rt_fw_scan(&g_rt);
        g_scan_busy = 0;
        log_scan_cache("assoc", src);
        if (src != IWM_OK)
            break;
        rc = iwm_assoc_select_band(&g_scan, g_ad.ssid, g_ad.ssid_length,
                                   g_ad.req_bssid_valid ? g_ad.req_bssid : 0,
                                   IWM_AS_MAX_KHZ, &g_as_bss);
    }
    if (rc != IWM_OK) {
        dprintf(0, "iwm: assoc select '%s' rc %d cache %u scans %u: no matching 2.4 GHz BSS\n",
                name, rc, (unsigned)g_scan.count, k);
        return;
    }
    dprintf(0,
        "iwm: assoc select '%s' bss %02x:%02x:%02x:%02x:%02x:%02x khz %lu rssi %ld priv %u ies %u of %u wmm %u ht %u\n",
        name, g_as_bss.bssid[0], g_as_bss.bssid[1], g_as_bss.bssid[2],
        g_as_bss.bssid[3], g_as_bss.bssid[4], g_as_bss.bssid[5],
        (ULONG)g_as_bss.ds_config_khz, (long)g_as_bss.rssi_dbm,
        (unsigned)g_as_bss.privacy, (unsigned)g_as_bss.ie_length,
        (unsigned)g_as_bss.ie_total, (unsigned)g_as_bss.wmm,
        (unsigned)g_as_bss.ht);
    iwm_assoc_init(&g_as, &g_ad, &g_as_ops, 0);
    g_as.ht = IWM_HT;
    g_sta.filter_or = IWM_RXPROMISC ? (IWM_MAC_FILTER_IN_PROMISC |
                                       IWM_MAC_FILTER_IN_CONTROL_AND_MGMT) : 0UL;
    g_as_ms = 0;
    g_as_resp_len = 0;
    rxdiag_reset();
    rc = iwm_assoc_start(&g_as, &g_as_bss, g_ad.ssid, g_ad.ssid_length, 0);
    for (us = 0; (g_as.state == IWM_AS_AUTH || g_as.state == IWM_AS_ASSOC) &&
                 us < IWM_AS_MAX_WAIT_US; us += 100UL) {
        (void)iwm_rx_drain(&g_rxq);
        d_delay(0, 100U);
        g_as_ms = us / 1000UL;
        iwm_assoc_tick(&g_as, (uint32_t)g_as_ms);
    }
    dprintf(0,
        "iwm: assoc rc %d state %u fail %u status %u aid %u tries %u auth %lu assoc %lu ignored %lu ms %lu mgmt rx %lu\n",
        rc, (unsigned)g_as.state, (unsigned)g_as.fail, (unsigned)g_as.status,
        (unsigned)g_as.aid, (unsigned)g_as.tries, (ULONG)g_as.tx_auth,
        (ULONG)g_as.tx_assoc, (ULONG)g_as.rx_ignored, g_as_ms,
        (ULONG)g_rt.mgmt_frames);
    log_txresp();
    if (g_as_resp_len)
        log_frame("assocresp", g_as_resp, g_as_resp_len);
    if (g_as.state != IWM_AS_UP) {
        unsigned first = g_sta.log_n;
        iwm_sta_teardown(&g_sta);
        log_sta_cmds(first);
        dprintf(0, "iwm: sta torn down after failed attempt\n");
    } else {
        run_dhcp_probe();
    }
}

static int run_scan_probe(void)
{
    unsigned i, j;
    unsigned first = g_rt.cmd_count;
    int rc;
    (void)iwm_d32_msi_rearm("probe");
    rc = iwm_rt_fw_scan_probe(&g_rt);
    log_rt_cmds(first);
    dprintf(0,
        "iwm: scanprobe rc %d stage %u failcmd %02x seq %04x done %u wait %lu us complete len %u %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x hist %u lost %lu mpdu %u\n",
        rc, (unsigned)g_rt.stage, (unsigned)g_rt.fail_cmd,
        (unsigned)g_rt.fail_seq, (unsigned)g_rt.scan_done,
        (ULONG)g_rt.scan_wait_us, (unsigned)g_rt.scan_complete_len,
        g_rt.scan_complete[0], g_rt.scan_complete[1], g_rt.scan_complete[2],
        g_rt.scan_complete[3], g_rt.scan_complete[4], g_rt.scan_complete[5],
        g_rt.scan_complete[6], g_rt.scan_complete[7], g_rt.scan_complete[8],
        g_rt.scan_complete[9], g_rt.scan_complete[10], g_rt.scan_complete[11],
        (unsigned)g_rt.hist_n, (ULONG)g_rt.hist_lost, (unsigned)g_rt.mpdu_n);
    for (i = 0; i < g_rt.hist_n; ++i)
        dprintf(0, "iwm: scanhist grp %02x cmd %02x count %lu maxlen %u\n",
                (unsigned)(g_rt.hist[i].key >> 8),
                (unsigned)(g_rt.hist[i].key & 0xffU),
                (ULONG)g_rt.hist[i].count, (unsigned)g_rt.hist[i].max_len);
    for (i = 0; i < g_rt.mpdu_n; ++i)
        for (j = 0; j < IWM_RT_MPDU_SAMPLE_LEN; j += 16U) {
            const uint8_t *b = &g_rt.mpdu[i][j];
            dprintf(0,
                "iwm: mpdu %u len %u +%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
                i, (unsigned)g_rt.mpdu_len[i], j, b[0], b[1], b[2], b[3],
                b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12],
                b[13], b[14], b[15]);
        }
    log_scan_cache("probe", rc);
    return rc;
}

/* Milestone C: load the REGULAR image, validate runtime ALIVE, then paging,
 * TX antennas, PHY-DB replay and PHY configuration.  The firmware session
 * stays LOADING (scan remains gated) until scan support exists. */
static void start_runtime_firmware(void)
{
    const uint32_t *e = g_loader.last_lmac_err;
    unsigned i;
    int rc;
    iwm_rt_fw_init(&g_rt, &g_ad, &g_rxq, &g_tx, &g_init);
    iwm_rt_fw_set_mgmt_cb(&g_rt, as_mgmt_rx, 0);
    iwm_sta_init(&g_sta, &g_rt, &g_tx);
    iwm_data_init(&g_data, &g_tx);
    iwm_d32_ndis_bind(&g_ndis, &g_ad);
    iwm_rt_fw_set_notif_hook(&g_rt, os2_notif, &g_sta);
    rc = iwm_rt_fw_prepare_paging(&g_rt);
    dprintf(0,
        "iwm: rt paging rc %d sep %u css %u data %u size %lu blocks %lu last %lu css %08lx pages %08lx\n",
        rc, (unsigned)g_rt.paging_sep, (unsigned)g_rt.css_sec,
        (unsigned)g_rt.data_sec, (ULONG)g_rt.paging_size,
        (ULONG)g_rt.paging_blocks, (ULONG)g_rt.pages_last,
        (ULONG)g_rt.css.paddr, (ULONG)g_rt.pages.paddr);
    if (rc != IWM_OK)
        return;
    rc = iwm_fw_loader_start(&g_loader, IWM_FW_SEC_RUNTIME);
    dprintf(0,
        "iwm: rt load rc %d stage %lu sections %lu bytes %lu dst %08lx aliveirq %lu aliventf %lu csr %08lx fh %08lx load %08lx rx %lu bad %lu closed %lu\n",
        rc, (ULONG)g_loader.stage, (ULONG)g_loader.loaded_sections,
        (ULONG)g_loader.loaded_bytes, (ULONG)g_loader.last_dst,
        (ULONG)g_loader.alive_interrupt, (ULONG)g_loader.alive_notification,
        (ULONG)g_loader.last_csr_int, (ULONG)g_loader.last_fh_int,
        (ULONG)g_loader.last_load_status, (ULONG)g_rxq.packets,
        (ULONG)g_rxq.malformed, (ULONG)g_rxq.last_closed);
    dprintf(0,
        "iwm: rt irqdbg seen %08lx calls %u alive@%u err@%u mask %08lx int %08lx msi %lu t10ms sb1 %08lx sb2 %08lx umacpc %08lx lmacpc %08lx\n",
        (ULONG)g_loader.irq_seen, (unsigned)g_loader.irq_calls,
        (unsigned)g_loader.alive_at, (unsigned)g_loader.err_at,
        (ULONG)d_rd(0, 0x00cUL), (ULONG)d_rd(0, 0x008UL),
        (ULONG)iwm_m1_irq_count,
        (ULONG)g_loader.early_sb[1][0], (ULONG)g_loader.early_sb[1][1],
        (ULONG)g_loader.early_sb[1][2], (ULONG)g_loader.early_sb[1][3]);
    /* No MSI has arrived since the INIT image was stopped (msi count frozen,
     * calls 0) although CSR_INT has unmasked causes pending.  UREG_CHICK is
     * the runtime prepare_card_hw() value pair: before > after the
     * MSI_ENABLE write (0x01000000 = MSI, 0x04000000 = MSI-X).  CSR_RESET
     * shows a STOP_MASTER (0x200) or MASTER_DISABLED (0x100) left behind by
     * iwm_init_fw_stop(). */
    dprintf(0,
        "iwm: rt intdbg chick %08lx>%08lx reset %08lx gp %08lx hwif %08lx wfpm %08lx>%08lx rc %d hpm %08lx fhint %08lx\n",
        (ULONG)g_ad.ureg_chick[0], (ULONG)g_ad.ureg_chick[1],
        (ULONG)d_rd(0, 0x020UL), (ULONG)d_rd(0, 0x024UL),
        (ULONG)d_rd(0, 0x000UL), (ULONG)g_ad.wfpm_ctrl[0],
        (ULONG)g_ad.wfpm_ctrl[1], (int)g_ad.wfpm_rc, (ULONG)g_ad.hpm_debug,
        (ULONG)d_rd(0, 0x010UL));
    iwm_d32_int_dump("rtload");
    if (rc != IWM_OK) {
        dprintf(0,
            "iwm: rt fail sb1 %08lx sb2 %08lx umacpc %08lx lmacpc %08lx lmacerr %08lx %08lx data1 %08lx log_pc %08lx umacerr %08lx %08lx\n",
            (ULONG)g_loader.last_sb[0], (ULONG)g_loader.last_sb[1],
            (ULONG)g_loader.last_sb[2], (ULONG)g_loader.last_sb[3],
            (ULONG)e[0], (ULONG)e[1], (ULONG)e[7], (ULONG)e[20],
            (ULONG)g_loader.last_umac_err[0],
            (ULONG)g_loader.last_umac_err[1]);
        return;
    }
    dprintf(0, "iwm: rt alive status %04x flags %04x lmac %08lx.%08lx scd %08lx\n",
            g_rxq.alive.status, g_rxq.alive.flags,
            (ULONG)g_rxq.alive.lmac_major, (ULONG)g_rxq.alive.lmac_minor,
            (ULONG)g_tx.scd_base);
    rc = iwm_rt_fw_run(&g_rt);
    dprintf(0,
        "iwm: rtseq rc %d stage %u failcmd %02x seq %04x phydb sent %u skipped %u bytes %lu phy %08lx flow %08lx event %08lx notif %lu\n",
        rc, (unsigned)g_rt.stage, (unsigned)g_rt.fail_cmd,
        (unsigned)g_rt.fail_seq, (unsigned)g_rt.phydb_sent,
        (unsigned)g_rt.phydb_skipped, (ULONG)g_rt.phydb_bytes,
        (ULONG)g_rt.phy_cfg, (ULONG)g_rt.flow_trigger,
        (ULONG)g_rt.event_trigger, (ULONG)g_rt.notif_count);
    log_rt_cmds(0);
    for (i = 0; i < IWM_RT_NOTIF_LOG && i < g_rt.notif_count; ++i)
        dprintf(0, "iwm: rt notif %u cmd %02lx grp %02lx len %lu\n", i,
                (ULONG)(g_rt.notif_log[i] & 0xffUL),
                (ULONG)((g_rt.notif_log[i] >> 8) & 0xffUL),
                (ULONG)(g_rt.notif_log[i] >> 16));
    if (rc == IWM_OK) {
        dprintf(0, "iwm: runtime firmware configured\n");
        rc = run_scan_probe();
        /* Runtime ALIVE, configuration and one complete scan: the firmware
         * session becomes RUNNING, which opens OID_802_11_BSSID_LIST_SCAN. */
        if (rc == IWM_OK) {
            rc = iwm_adapter_fw_alive(&g_ad);
            dprintf(0, "iwm: firmware running rc %d state %u fw %u\n", rc,
                    (unsigned)g_ad.state, (unsigned)g_ad.firmware.state);
            iwm_d32_int_dump("running");
        }
    }
}

static void run_init_sequence(void)
{
    struct iwm_tx_debug txd;
    unsigned i;
    int rc;
    iwm_init_fw_init(&g_init, &g_ad, &g_rxq, &g_tx);
    g_msi_off_seen = 0;
    g_msi_watch = 1;
    rc = iwm_init_fw_run(&g_init);
    g_msi_watch = 0;
    iwm_tx_debug_snapshot(&g_tx, &txd);
    dprintf(0,
        "iwm: initseq rc %d stage %u failcmd %02x seq %04x tx_ant %lx phy %08lx flow %08lx event %08lx nvm %lu phydb %u/%lu complete %u notif %lu other %lu last %02x\n",
        rc, (unsigned)g_init.stage, (unsigned)g_init.fail_cmd,
        (unsigned)g_init.fail_seq, (ULONG)g_init.tx_ant,
        (ULONG)g_init.phy_cfg, (ULONG)g_init.flow_trigger,
        (ULONG)g_init.event_trigger, (ULONG)g_init.nvm_used,
        (unsigned)g_init.phydb_count, (ULONG)g_init.phydb_used,
        (unsigned)g_init.init_complete, (ULONG)g_init.notif_count,
        (ULONG)g_init.notif_other, (unsigned)g_init.last_notif);
    for (i = 0; i < IWM_INIT_OTHER_LOG && i < g_init.notif_other; ++i)
        dprintf(0, "iwm: init other %u cmd %02lx grp %02lx len %lu\n", i,
                (ULONG)(g_init.other_log[i] & 0xffUL),
                (ULONG)((g_init.other_log[i] >> 8) & 0xffUL),
                (ULONG)(g_init.other_log[i] >> 16));
    for (i = 0; i < IWM_NVM_NUM_SECTIONS; ++i)
        dprintf(0, "iwm: nvm sec %u len %u chunks %u status %04x\n",
                i, (unsigned)g_init.nvm[i].length,
                (unsigned)g_init.nvm[i].chunks,
                (unsigned)g_init.nvm[i].status);
    dprintf(0,
        "iwm: initseq mcc rc %d len %u %02x%02x%02x%02x %02x%02x%02x%02x tx rd %08lx wr %08lx rx pkts %lu bad %lu\n",
        (int)g_init.mcc_rc, (unsigned)g_init.mcc_len,
        g_init.mcc_resp[0], g_init.mcc_resp[1], g_init.mcc_resp[2],
        g_init.mcc_resp[3], g_init.mcc_resp[4], g_init.mcc_resp[5],
        g_init.mcc_resp[6], g_init.mcc_resp[7],
        (ULONG)txd.scd_cmd_rdptr, (ULONG)txd.scd_cmd_wrptr,
        (ULONG)g_rxq.packets, (ULONG)g_rxq.malformed);
    iwm_d32_int_dump("initseq");
    if (rc == IWM_OK) {
        rc = iwm_init_fw_stop(&g_init);
        dprintf(0, "iwm: init firmware stopped rc %d\n", rc);
        iwm_d32_int_dump("initstop");
        (void)iwm_d32_msi_rearm("initstop");
        if (rc == IWM_OK)
            start_runtime_firmware();
    }
}

static int start_init_firmware(void)
{
    int rc;

    rc = iwm_fw_loader_start(&g_loader, IWM_FW_SEC_INIT);
    dprintf(0,
        "iwm: init load rc %d stage %lu sections %lu bytes %lu dst %08lx aliveirq %lu aliventf %lu csr %08lx fh %08lx load %08lx ints %lu rx %lu bad %lu closed %lu\n",
        rc, (ULONG)g_loader.stage, (ULONG)g_loader.loaded_sections,
        (ULONG)g_loader.loaded_bytes, (ULONG)g_loader.last_dst,
        (ULONG)g_loader.alive_interrupt, (ULONG)g_loader.alive_notification,
        (ULONG)g_loader.last_csr_int,
        (ULONG)g_loader.last_fh_int, (ULONG)g_loader.last_load_status,
        (ULONG)iwm_m1_irq_count, (ULONG)g_rxq.packets,
        (ULONG)g_rxq.malformed, (ULONG)g_rxq.last_closed);
    if (rc != IWM_OK) {
        dprintf(0,
            "iwm: dbg trg %08lx direct %08lx gp %08lx rfh cfg %08lx act %08lx widx %08lx ridx %08lx stts %08lx\n",
            (ULONG)g_rxq.widx_readback, (ULONG)g_rxq.widx_direct,
            (ULONG)g_loader.last_gp_cntrl, (ULONG)g_loader.last_rfh[0],
            (ULONG)g_loader.last_rfh[1], (ULONG)g_loader.last_rfh[2],
            (ULONG)g_loader.last_rfh[3], (ULONG)g_loader.last_rfh[4]);
        dprintf(0,
            "iwm: rxbuf0 %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx used %08lx %08lx %08lx %08lx\n",
            (ULONG)g_loader.last_rxbuf[0], (ULONG)g_loader.last_rxbuf[1],
            (ULONG)g_loader.last_rxbuf[2], (ULONG)g_loader.last_rxbuf[3],
            (ULONG)g_loader.last_rxbuf[4], (ULONG)g_loader.last_rxbuf[5],
            (ULONG)g_loader.last_rxbuf[6], (ULONG)g_loader.last_rxbuf[7],
            (ULONG)g_loader.last_used[0], (ULONG)g_loader.last_used[1],
            (ULONG)g_loader.last_used[2], (ULONG)g_loader.last_used[3]);
        dprintf(0,
            "iwm: txdbg state %u/%u/%u kw %08lx cbbc %08lx fh0 %08lx fh7 %08lx chick %08lx scdbase %08lx bc %08lx fact %08lx rd %08lx wr %08lx st %08lx hbus %08lx\n",
            (unsigned)g_loader.last_tx_state[0],
            (unsigned)g_loader.last_tx_state[1],
            (unsigned)g_loader.last_tx_state[2],
            (ULONG)g_loader.last_tx[0], (ULONG)g_loader.last_tx[1],
            (ULONG)g_loader.last_tx[2], (ULONG)g_loader.last_tx[3],
            (ULONG)g_loader.last_tx[4], (ULONG)g_loader.last_tx[5],
            (ULONG)g_loader.last_tx[6], (ULONG)g_loader.last_tx[7],
            (ULONG)g_loader.last_tx[8], (ULONG)g_loader.last_tx[9],
            (ULONG)g_loader.last_tx[10], (ULONG)g_loader.last_tx[11]);
        dprintf(0,
            "iwm: sbdbg hwif %08lx sb1 %08lx sb2 %08lx umacpc %08lx lmacpc %08lx hpm %08lx\n",
            (ULONG)g_loader.last_hw_if, (ULONG)g_loader.last_sb[0],
            (ULONG)g_loader.last_sb[1], (ULONG)g_loader.last_sb[2],
            (ULONG)g_loader.last_sb[3], (ULONG)g_ad.hpm_debug);
        {
            const uint32_t *e = g_loader.last_lmac_err;
            const uint32_t *u = g_loader.last_umac_err;
            dprintf(0,
                "iwm: lmacerr @%08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n",
                (ULONG)g_loader.last_lmac_err_addr, (ULONG)e[0], (ULONG)e[1],
                (ULONG)e[2], (ULONG)e[3], (ULONG)e[4], (ULONG)e[5],
                (ULONG)e[6], (ULONG)e[7]);
            dprintf(0,
                "iwm: umacerr @%08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n",
                (ULONG)g_loader.last_umac_err_addr, (ULONG)u[0], (ULONG)u[1],
                (ULONG)u[2], (ULONG)u[3], (ULONG)u[4], (ULONG)u[5],
                (ULONG)u[6], (ULONG)u[7]);
            dprintf(0,
                "iwm: lmacerr2 data2 %08lx data3 %08lx bcon %08lx tsf %08lx.%08lx gp1 %08lx gp2 %08lx rev %08lx\n",
                (ULONG)e[8], (ULONG)e[9], (ULONG)e[10], (ULONG)e[12],
                (ULONG)e[11], (ULONG)e[13], (ULONG)e[14], (ULONG)e[15]);
            dprintf(0,
                "iwm: lmacerr3 major %08lx minor %08lx hw %08lx brd %08lx log_pc %08lx fp %08lx sp %08lx hcmd %08lx\n",
                (ULONG)e[16], (ULONG)e[17], (ULONG)e[18], (ULONG)e[19],
                (ULONG)e[20], (ULONG)e[21], (ULONG)e[22], (ULONG)e[23]);
            dprintf(0,
                "iwm: umacerr2 data2 %08lx data3 %08lx major %08lx minor %08lx fp %08lx sp %08lx hcmd %08lx isr %08lx\n",
                (ULONG)u[7], (ULONG)u[8], (ULONG)u[9], (ULONG)u[10],
                (ULONG)u[11], (ULONG)u[12], (ULONG)u[13], (ULONG)u[14]);
        }
    }
    dprintf(0,
        "iwm: irqdbg seen %08lx calls %u alive@%u err@%u t0 sb1 %08lx sb2 %08lx umacpc %08lx lmacpc %08lx t10ms sb1 %08lx sb2 %08lx umacpc %08lx lmacpc %08lx\n",
        (ULONG)g_loader.irq_seen, (unsigned)g_loader.irq_calls,
        (unsigned)g_loader.alive_at, (unsigned)g_loader.err_at,
        (ULONG)g_loader.early_sb[0][0], (ULONG)g_loader.early_sb[0][1],
        (ULONG)g_loader.early_sb[0][2], (ULONG)g_loader.early_sb[0][3],
        (ULONG)g_loader.early_sb[1][0], (ULONG)g_loader.early_sb[1][1],
        (ULONG)g_loader.early_sb[1][2], (ULONG)g_loader.early_sb[1][3]);
    dprintf(0, "iwm: bootdbg wfpm %08lx>%08lx rc %d hpm %08lx>%08lx chick %08lx>%08lx\n",
            (ULONG)g_ad.wfpm_ctrl[0], (ULONG)g_ad.wfpm_ctrl[1],
            (int)g_ad.wfpm_rc, (ULONG)g_ad.hpm_debug,
            (ULONG)g_ad.hpm_after, (ULONG)g_ad.ureg_chick[0],
            (ULONG)g_ad.ureg_chick[1]);
    iwm_d32_trace_dump();
    iwm_d32_int_dump("initload");
    if (g_loader.alive_notification)
        dprintf(0, "iwm: alive status %04x flags %04x lmac %08lx.%08lx\n",
                g_rxq.alive.status, g_rxq.alive.flags,
                (ULONG)g_rxq.alive.lmac_major,
                (ULONG)g_rxq.alive.lmac_minor);
    if (rc == IWM_OK)
        run_init_sequence();
    return rc;
}

/* One summary line plus one line per section after COMMIT, or the exact
 * record the parser refused. Read it back with "type wrnd32$". */
static void log_commit(int rc)
{
    const struct iwm_fw_image *im = &g_ad.firmware.catalog;
    unsigned i;

    if (rc != IWM_OK) {
        dprintf(0, "iwm: parse refused rc %d reason %lu at %lu type %lu len %lu\n",
                rc, (ULONG)im->diag.reason, (ULONG)im->diag.pos,
                (ULONG)im->diag.type, (ULONG)im->diag.length);
        return;
    }
    dprintf(0, "iwm: image ver %lu build %08lx cpus %lu sections %lu init %lu rt %lu phy %08lx errtab %08lx/%08lx paging %lu calib rt %08lx/%08lx init %08lx/%08lx\n",
            (ULONG)im->version, (ULONG)im->build, (ULONG)im->num_cpus,
            (ULONG)im->section_count, (ULONG)im->init_count,
            (ULONG)im->runtime_count, (ULONG)im->phy_config,
            (ULONG)im->lmac_err_addr, (ULONG)im->umac_err_addr,
            (ULONG)im->paging_mem_size,
            (ULONG)im->calib_flow[IWM_UCODE_TYPE_REGULAR],
            (ULONG)im->calib_event[IWM_UCODE_TYPE_REGULAR],
            (ULONG)im->calib_flow[IWM_UCODE_TYPE_INIT],
            (ULONG)im->calib_event[IWM_UCODE_TYPE_INIT]);
    for (i = 0; i < im->section_count; ++i)
        dprintf(0, "iwm: sec %lu type %lu dst %08lx len %lu flags %02lx\n",
                (ULONG)i, (ULONG)im->section[i].tlv_type,
                (ULONG)im->section[i].dst_addr,
                (ULONG)im->section[i].data_length,
                (ULONG)im->section[i].flags);
}

void iwm_d32_ioctl(REQPACKET *prp)
{
    static struct iwm_os2_ioctl req;   /* static: see W112 note */
    void *data = 0;
    ULONG *parm = 0;
    UCHAR cat = prp->ioctl.bCategory;
    int rc;

    if (cat != IWM_IOCTL_CATEGORY && cat != GENMAC_CATEGORY) {
        prp->usStatus = RPDONE | RPERR_BADCOMMAND;
        return;
    }
    if (!g_attached) {
        dprintf(0, "iwm: ioctl cat %x before attach\n", (int)cat);
        prp->usStatus = RPDONE | RPERR_NOTREADY;
        return;
    }
    memset(&req, 0, sizeof(req));
    req.category = cat;
    req.function = prp->ioctl.bFunction;

    if (prp->ioctl.usDataLen) {
        data = Far16ToFlat(prp->ioctl.pvData);
        if (!data) {
            prp->usStatus = RPDONE | RPERR_PARAMETER;
            return;
        }
    }
    req.data = data;
    req.data_length = prp->ioctl.usDataLen;

    /* GenMAC: OID in parameters[0]; bytes done and NDIS status come back
     * in parameters[2] and [3]. */
    if (cat == GENMAC_CATEGORY) {
        if (prp->ioctl.usParmLen < sizeof(req.parameters)) {
            dprintf(0, "iwm: genmac fn %x parm len %u too short\n",
                    (int)req.function, (unsigned)prp->ioctl.usParmLen);
            prp->usStatus = RPDONE | RPERR_PARAMETER;
            return;
        }
        parm = (ULONG *)Far16ToFlat(prp->ioctl.pvParm);
        if (!parm) {
            prp->usStatus = RPDONE | RPERR_PARAMETER;
            return;
        }
        memcpy(req.parameters, parm, sizeof(req.parameters));
    }

    iwm_d32_lock();
    g_task_time = 1;            /* RX now drained by this thread only */
    iwm_d32_unlock();
    g_wait_us = 0;
    g_ioctl_wait_us = 0;
    handle_deauth();
    rc = iwm_os2_ioctl_dispatch(&g_ad, &req);
    if (cat == GENMAC_CATEGORY && rc == IWM_OK &&
        req.function == GENMAC_WRAPPER_OID_SET &&
        req.parameters[0] == OID_802_11_BSSID_LIST_SCAN)
        run_xwlan_scan();
    if (cat == GENMAC_CATEGORY && rc == IWM_OK && g_ad.assoc_request) {
        dprintf(0, "iwm: assoc request %u from oid %08lx\n",
                (unsigned)g_ad.assoc_request, (ULONG)req.parameters[0]);
        run_associate();
    }

    if (parm)
        memcpy(parm, req.parameters, sizeof(req.parameters));
    if (req.data_actual && req.data_actual <= prp->ioctl.usDataLen)
        prp->ioctl.usDataLen = (USHORT)req.data_actual;

    if (cat == GENMAC_CATEGORY) {
        /* XWLAN polls about a dozen GET OIDs every few seconds; logging each
         * successful one filled the 64 KB WRND32 log before the connect
         * attempt (d12, d13).  Successful GETs are counted, not logged. */
        static ULONG quiet_gets;
        if (req.function == GENMAC_WRAPPER_OID_GET && rc == IWM_OK &&
            req.parameters[3] == 0) {
            ++quiet_gets;
        } else {
            dprintf(0, "iwm: genmac fn %x oid %08lx len %lu rc %d done %lu st %08lx gets %lu\n",
                    (int)req.function, (ULONG)req.parameters[0],
                    (ULONG)req.data_length, rc, (ULONG)req.parameters[2],
                    (ULONG)req.parameters[3], quiet_gets);
        }
        /* d13: SET OID_802_11_SSID returned rc 0 twice but no "assoc select"
         * line followed, so no association request was recorded.  Show what
         * arrived and the state that decided it (fw 4 = RUNNING). */
        if (req.function == GENMAC_WRAPPER_OID_SET &&
            req.parameters[0] == OID_802_11_SSID && data &&
            req.data_length >= 4U) {
            static char sname[33];   /* static: see W112 note */
            ULONG slen = iwm_get_le32((const uint8_t *)data);
            unsigned k;
            for (k = 0; k < slen && k < 32U && 4U + k < req.data_length; ++k) {
                UCHAR ch = ((const UCHAR *)data)[4U + k];
                sname[k] = (ch < 0x20 || ch > 0x7e) ? '?' : (char)ch;
            }
            sname[k] = 0;
            dprintf(0, "iwm: genmac set ssid len %lu '%s' stored %u fw %u request %u\n",
                    slen, sname, (unsigned)g_ad.ssid_length,
                    (unsigned)g_ad.firmware.state,
                    (unsigned)g_ad.assoc_request);
        }
        /* A rejected SET ends XWLAN's connect sequence (d9: REMOVE_WEP), so
         * record the payload of whatever was refused. */
        if (req.function == GENMAC_WRAPPER_OID_SET && rc != IWM_OK && data) {
            static UCHAR rej[8];   /* static: see W112 note */
            unsigned n = req.data_length < 8U ? (unsigned)req.data_length : 8U;
            memset(rej, 0, sizeof(rej));
            memcpy(rej, data, n);
            dprintf(0, "iwm: genmac set rejected oid %08lx data %02x%02x%02x%02x %02x%02x%02x%02x\n",
                    (ULONG)req.parameters[0], rej[0], rej[1], rej[2], rej[3],
                    rej[4], rej[5], rej[6], rej[7]);
        }
    } else {
        if (req.function == IWM_IOCTL_FW_COMMIT) {
            log_commit(rc);
            /* COMMIT reports the transfer (length, CRC, TLV parse). The INIT
             * boot result is logged and kept in firmware.last_error for
             * FW_STATUS; a failed boot still leaves the session FAILED and
             * scan gated, but no longer looks like a failed copy. */
            if (rc == IWM_OK && g_ad.firmware.state == IWM_FW_LOADING) {
                int boot = start_init_firmware();
                if (boot != IWM_OK)
                    dprintf(0, "iwm: transfer ok, init boot failed rc %d\n",
                            boot);
            }
        }
        if (req.function != IWM_IOCTL_FW_DATA || rc != IWM_OK)
            dprintf(0, "iwm: fw fn %d len %lu rc %d state %d accepted %lu\n",
                    (int)req.function, (ULONG)req.data_length, rc,
                    (int)g_ad.firmware.state, (ULONG)g_ad.firmware.accepted);
    }

    handle_deauth();
    iwm_d32_lock();
    g_task_time = 0;
    iwm_d32_unlock();
    /* Frames that arrived while this IOCTL ran and were not drained by it
     * would otherwise wait for the next interrupt. */
    isr_drain();
    fw_health();
    if (g_ioctl_wait_us >= 100000UL)
        dprintf(0, "iwm: ioctl cat %x fn %x waited %lu ms yields %lu\n",
                (int)cat, (int)req.function, g_ioctl_wait_us / 1000UL,
                g_yields);

    /* A syntactically valid GenMAC request reports protocol failures through
     * parameters[3].  Returning an OS/2 request-packet error as well hides the
     * NDIS status from XWLAN and turns ADAPTER_NOT_READY into a generic IOCTL
     * failure. */
    if (cat == GENMAC_CATEGORY && req.parameters[3] != 0) {
        prp->usStatus = RPDONE;
        return;
    }

    switch (rc) {
    case IWM_OK:
        prp->usStatus = RPDONE;
        break;
    case IWM_EINVAL:
    case IWM_EOVERFLOW:
        prp->usStatus = RPDONE | RPERR_PARAMETER;
        break;
    case IWM_ENOTSUP:
        /* An unsupported OID is a normal GenMAC answer: the NDIS status in
         * parameters[3] says NOT_SUPPORTED, so let XWLAN read it. */
        prp->usStatus = cat == GENMAC_CATEGORY ? RPDONE
                                               : (RPDONE | RPERR_BADCOMMAND);
        break;
    default:
        prp->usStatus = RPDONE | RPERR_GENERAL;
        break;
    }
}


