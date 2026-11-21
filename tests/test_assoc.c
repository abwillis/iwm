/* SPDX-License-Identifier: GPL-2.0-only */
/* 802.11 open authentication / association, BSS selection and GenMAC wiring. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "iwm_assoc.h"
#include "iwm_test_beacon.h"

static const uint8_t OWN[6]  = { 0xd4, 0x3b, 0x04, 0xc3, 0x04, 0x74 };
static const uint8_t AP1[6]  = { 0xf0, 0x1d, 0x2d, 0x92, 0x32, 0xe5 };
static const uint8_t AP2[6]  = { 0x1c, 0xd1, 0xe0, 0x7a, 0x35, 0x4a };
static const uint8_t OTHER[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };

struct mock {
    int prepare_rc;
    int tx_rc;
    unsigned prepares, txs, ups, downs;
    uint8_t last[IWM_ASSOC_FRAME_MAX];
    uint16_t last_len;
    uint8_t fc0[16];
    uint16_t aid;
};

static int m_prepare(void *c, const struct iwm_scan_result *bss)
{
    struct mock *m = (struct mock *)c;
    (void)bss;
    ++m->prepares;
    return m->prepare_rc;
}

static int m_tx(void *c, const uint8_t *f, uint16_t n)
{
    struct mock *m = (struct mock *)c;
    if (m->tx_rc != IWM_OK)
        return m->tx_rc;
    if (m->txs < sizeof(m->fc0))
        m->fc0[m->txs] = f[0];
    ++m->txs;
    memcpy(m->last, f, n);
    m->last_len = n;
    return IWM_OK;
}

static void m_up(void *c, uint16_t aid)
{
    struct mock *m = (struct mock *)c;
    ++m->ups;
    m->aid = aid;
}

static void m_down(void *c)
{
    struct mock *m = (struct mock *)c;
    ++m->downs;
}

static const struct iwm_assoc_ops ops = { m_prepare, m_tx, m_up, m_down };

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/* AP-to-station management frame with a 6-byte body (30 bytes). */
static uint16_t mk(uint8_t *f, uint8_t fc0, const uint8_t *da,
                   const uint8_t *bssid, uint16_t w0, uint16_t w1, uint16_t w2)
{
    memset(f, 0, 32);
    f[0] = fc0;
    memcpy(f + 4, da, 6);
    memcpy(f + 10, bssid, 6);
    memcpy(f + 16, bssid, 6);
    put16(f + 24, w0);
    put16(f + 26, w1);
    put16(f + 28, w2);
    return 30;
}

static struct iwm_scan_result make_bss(const uint8_t *bssid, const char *ssid,
                                       int rssi, int privacy, int five,
                                       uint16_t cap)
{
    struct iwm_scan_result r;
    memset(&r, 0, sizeof(r));
    memcpy(r.bssid, bssid, 6);
    r.ssid_length = (uint8_t)strlen(ssid);
    memcpy(r.ssid, ssid, r.ssid_length);
    r.rssi_dbm = rssi;
    r.privacy = (uint8_t)privacy;
    r.infrastructure_mode = IWM_INFRASTRUCTURE_BSS;
    r.network_type = (uint8_t)(five ? IWM_NETWORK_TYPE_OFDM5 :
                                      IWM_NETWORK_TYPE_OFDM24);
    r.ds_config_khz = five ? 5180000UL : 2412000UL;
    r.ie_length = 12;
    r.ies[10] = (uint8_t)cap;
    r.ies[11] = (uint8_t)(cap >> 8);
    return r;
}

static void setup(struct iwm_adapter *a, struct iwm_assoc *as, struct mock *m)
{
    iwm_adapter_init(a, NULL, NULL);
    assert(iwm_adapter_set_mac(a, OWN) == IWM_OK);
    memset(m, 0, sizeof(*m));
    iwm_assoc_init(as, a, &ops, m);
}

static void oid_set(struct iwm_adapter *a, uint32_t oid, void *d, uint32_t n,
                    int expect)
{
    struct iwm_oid_request r;
    memset(&r, 0, sizeof(r));
    r.oid = oid;
    r.data = d;
    r.data_length = n;
    assert(iwm_genmac_oid_set(a, &r) == expect);
}

int main(void)
{
    static struct iwm_adapter a;
    static struct iwm_assoc as;
    static struct mock m;
    static struct iwm_scan_cache cache;
    static struct iwm_scan_result bss, sel;
    static uint8_t f[64], buf[1024];
    uint32_t now, len;
    unsigned i;
    int rc;

    /* ---- BSS selection: strongest match, infrastructure only. */
    memset(&cache, 0, sizeof(cache));
    cache.result[0] = make_bss(AP2, "lab", -70, 0, 0, 0x0001);
    cache.result[1] = make_bss(AP1, "lab", -50, 0, 1, 0x0001);
    cache.result[2] = make_bss(OTHER, "lab2", -30, 0, 0, 0x0001);
    cache.result[3] = make_bss(OTHER, "", -20, 0, 0, 0x0001);
    cache.result[4] = make_bss(OTHER, "lab", -10, 0, 0, 0x0001);
    cache.result[4].infrastructure_mode = IWM_INFRASTRUCTURE_IBSS;
    cache.count = 5;
    assert(iwm_assoc_select(&cache, (const uint8_t *)"lab", 3, NULL, &sel) == IWM_OK);
    assert(memcmp(sel.bssid, AP1, 6) == 0 && sel.rssi_dbm == -50);
    assert(iwm_assoc_select(&cache, (const uint8_t *)"lab", 3, AP2, &sel) == IWM_OK);
    assert(memcmp(sel.bssid, AP2, 6) == 0);
    assert(iwm_assoc_select(&cache, NULL, 0, AP1, &sel) == IWM_OK);
    assert(iwm_assoc_select(&cache, (const uint8_t *)"nope", 4, NULL, &sel) == IWM_ENODEV);
    assert(iwm_assoc_select(&cache, (const uint8_t *)"la", 2, NULL, &sel) == IWM_ENODEV);
    assert(iwm_assoc_select(&cache, NULL, 0, NULL, &sel) == IWM_EINVAL);
    assert(iwm_assoc_select(&cache, (const uint8_t *)"lab", 3, NULL, NULL) == IWM_EINVAL);

    /* ---- Exact authentication frame (30 bytes) and sequence counting. */
    setup(&a, &as, &m);
    bss = make_bss(AP1, "lab", -50, 0, 0, 0x0421);
    assert(iwm_assoc_start(&as, &bss, (const uint8_t *)"lab", 3, 1000) == IWM_OK);
    {
        static const uint8_t want[30] = {
            0xb0, 0x00, 0x00, 0x00,
            0xf0, 0x1d, 0x2d, 0x92, 0x32, 0xe5,
            0xd4, 0x3b, 0x04, 0xc3, 0x04, 0x74,
            0xf0, 0x1d, 0x2d, 0x92, 0x32, 0xe5,
            0x00, 0x00,
            0x00, 0x00, 0x01, 0x00, 0x00, 0x00
        };
        assert(m.txs == 1 && m.last_len == 30 && memcmp(m.last, want, 30) == 0);
    }
    assert(as.state == IWM_AS_AUTH && as.tries == 1 && as.deadline_ms == 1300);

    /* ---- Association request, 2.4 GHz: capability, rates split 8 + 4. */
    assert(iwm_assoc_build_assoc_req(&as) == 24U + 4U + 5U + 10U + 6U);
    assert(as.frame[0] == 0x00 && as.frame[1] == 0);
    assert((as.frame[22] | (as.frame[23] << 8)) == 0x0010);      /* seq 1 */
    assert((as.frame[24] | (as.frame[25] << 8)) == 0x0421);      /* ESS|SP|SS */
    assert((as.frame[26] | (as.frame[27] << 8)) == IWM_ASSOC_LISTEN_INTERVAL);
    assert(as.frame[28] == 0 && as.frame[29] == 3 &&
           memcmp(as.frame + 30, "lab", 3) == 0);
    assert(as.frame[33] == 1 && as.frame[34] == 8 && as.frame[35] == 2 &&
           as.frame[42] == 36);
    assert(as.frame[43] == 50 && as.frame[44] == 4 && as.frame[45] == 48 &&
           as.frame[48] == 108);
    /* The requested SSID, not the beacon's, goes on the air (hidden AP). */
    bss = make_bss(AP1, "", -50, 0, 1, 0x0001);
    setup(&a, &as, &m);
    assert(iwm_assoc_start(&as, &bss, (const uint8_t *)"secret", 6, 0) == IWM_OK);
    assert(iwm_assoc_build_assoc_req(&as) == 24U + 4U + 8U + 10U);
    assert(as.frame[29] == 6 && memcmp(as.frame + 30, "secret", 6) == 0);
    assert((as.frame[24] | (as.frame[25] << 8)) == 0x0001);      /* ESS only */
    /* 5 GHz: eight OFDM rates, no extended rates, no CCK. */
    assert(as.frame[36] == 1 && as.frame[37] == 8 && as.frame[38] == 12 &&
           as.frame[45] == 108 && as.frame_len == 46U);
    /* Deauthentication: reason 3. */
    assert(iwm_assoc_build_deauth(&as, IWM_80211_REASON_LEAVING) == 26U);
    assert(as.frame[0] == 0xc0 && as.frame[24] == 3 && as.frame[25] == 0);

    /* ---- WMM: announced only when the BSS's IEs carry a WMM information or
     * parameter element (00:50:f2 type 2); the request grows by 9 bytes. */
    {
        static const uint8_t wmm[9] = { 221, 7, 0x00, 0x50, 0xf2, 2, 0, 1, 0 };
        struct iwm_scan_result wb = make_bss(AP1, "lab", -50, 0, 0, 0x0421);
        uint16_t plain;
        setup(&a, &as, &m);
        assert(iwm_assoc_start(&as, &wb, (const uint8_t *)"lab", 3, 0) == IWM_OK);
        plain = iwm_assoc_build_assoc_req(&as);
        /* SSID element, then WMM information (subtype 0, length 7). */
        wb.ies[12] = 0; wb.ies[13] = 3; memcpy(wb.ies + 14, "lab", 3);
        memcpy(wb.ies + 17, wmm, 9);
        wb.ie_length = 26;
        setup(&a, &as, &m);
        assert(iwm_assoc_start(&as, &wb, (const uint8_t *)"lab", 3, 0) == IWM_OK);
        assert(iwm_assoc_build_assoc_req(&as) == plain + 9U);
        assert(memcmp(as.frame + plain, wmm, 9) == 0);
        /* WMM parameter element (subtype 1, length 24) counts too. */
        wb.ies[17 + 1] = 24; wb.ies[17 + 6] = 1;
        wb.ie_length = (uint16_t)(17 + 2 + 24);
        setup(&a, &as, &m);
        assert(iwm_assoc_start(&as, &wb, (const uint8_t *)"lab", 3, 0) == IWM_OK);
        assert(iwm_assoc_build_assoc_req(&as) == plain + 9U);
        /* A different vendor type, a wrong subtype and an element running past
         * the IE block do not. */
        wb.ies[17 + 5] = 4;
        wb.ie_length = 26;
        wb.ies[17 + 1] = 7;
        setup(&a, &as, &m);
        assert(iwm_assoc_start(&as, &wb, (const uint8_t *)"lab", 3, 0) == IWM_OK);
        assert(iwm_assoc_build_assoc_req(&as) == plain);
        wb.ies[17 + 5] = 2; wb.ies[17 + 6] = 2;
        setup(&a, &as, &m);
        assert(iwm_assoc_start(&as, &wb, (const uint8_t *)"lab", 3, 0) == IWM_OK);
        assert(iwm_assoc_build_assoc_req(&as) == plain);
        wb.ies[17 + 6] = 0; wb.ies[17 + 1] = 200;
        setup(&a, &as, &m);
        assert(iwm_assoc_start(&as, &wb, (const uint8_t *)"lab", 3, 0) == IWM_OK);
        assert(iwm_assoc_build_assoc_req(&as) == plain);
    }

    /* ---- Rates: the intersection with the BSS's rate set.  d27 log: the AP
     * advertises 9 12* 18 24 36 48 54 Mb/s; Linux announced exactly those seven
     * (without the basic-rate bit), the driver announced a fixed 12-rate table
     * with CCK and 6 Mb/s. */
    {
        static const uint8_t lwguest[7] = { 0x12, 0x98, 0x24, 0x30, 0x48, 0x60, 0x6c };
        static const uint8_t linux_ie[9] = { 1, 7, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6c };
        static const uint8_t bg[12] = { 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24,
                                        0x30, 0x48, 0x60, 0x6c };
        static const uint8_t a5[7] = { 0x8c, 0x12, 0x98, 0x24, 0xff, 0x82, 0x24 };
        struct iwm_scan_result rb = make_bss(AP1, "lab", -50, 0, 0, 0x0421);
        uint16_t l;
        memcpy(rb.rates, lwguest, sizeof(lwguest));
        setup(&a, &as, &m);
        assert(iwm_assoc_start(&as, &rb, (const uint8_t *)"lab", 3, 0) == IWM_OK);
        l = iwm_assoc_build_assoc_req(&as);
        assert(l == 24U + 4U + 5U + 9U);
        assert(memcmp(as.frame + 33, linux_ie, 9) == 0);
        /* b/g AP: the legacy table without basic bits, split 8 + 4. */
        rb = make_bss(AP1, "lab", -50, 0, 0, 0x0421);
        memcpy(rb.rates, bg, sizeof(bg));
        setup(&a, &as, &m);
        assert(iwm_assoc_start(&as, &rb, (const uint8_t *)"lab", 3, 0) == IWM_OK);
        assert(iwm_assoc_build_assoc_req(&as) == 24U + 4U + 5U + 10U + 6U);
        assert(as.frame[33] == 1 && as.frame[34] == 8 && as.frame[35] == 2 &&
               as.frame[42] == 36 && as.frame[43] == 50 && as.frame[44] == 4 &&
               as.frame[45] == 48 && as.frame[48] == 108);
        /* 5 GHz: CCK, a membership selector (0xff) and a duplicate are dropped. */
        rb = make_bss(AP1, "lab", -50, 0, 1, 0x0421);
        memcpy(rb.rates, a5, sizeof(a5));
        setup(&a, &as, &m);
        assert(iwm_assoc_start(&as, &rb, (const uint8_t *)"lab", 3, 0) == IWM_OK);
        assert(iwm_assoc_build_assoc_req(&as) == 24U + 4U + 5U + 2U + 4U);
        assert(as.frame[33] == 1 && as.frame[34] == 4 && as.frame[35] == 12 &&
               as.frame[36] == 18 && as.frame[37] == 24 && as.frame[38] == 36);
        /* Nothing usable in the cache entry: the fixed table is the fallback. */
        rb = make_bss(AP1, "lab", -50, 0, 0, 0x0421);
        rb.rates[0] = 0xff;
        setup(&a, &as, &m);
        assert(iwm_assoc_start(&as, &rb, (const uint8_t *)"lab", 3, 0) == IWM_OK);
        assert(iwm_assoc_build_assoc_req(&as) == 24U + 4U + 5U + 10U + 6U);
    }

    /* ---- HT20 announcement (policy as->ht): only when the BSS has an HT
     * capabilities element; placed before the WMM element. */
    {
        static const uint8_t ht20[28] = {
            45, 26, 0xa5, 0x01, 0x17,
            0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x90, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00
        };
        static const uint8_t wmm[9] = { 221, 7, 0x00, 0x50, 0xf2, 2, 0, 1, 0 };
        struct iwm_scan_result hb = make_bss(AP1, "lab", -50, 0, 0, 0x0421);
        uint16_t plain, l;
        setup(&a, &as, &m);
        assert(iwm_assoc_start(&as, &hb, (const uint8_t *)"lab", 3, 0) == IWM_OK);
        plain = iwm_assoc_build_assoc_req(&as);
        hb.ies[12] = 45; hb.ies[13] = 26;                 /* AP HT capabilities */
        memset(hb.ies + 14, 0x5a, 26);
        memcpy(hb.ies + 40, wmm, 9);
        hb.ie_length = 49;
        /* policy off: nothing announced, WMM only */
        setup(&a, &as, &m);
        assert(iwm_assoc_start(&as, &hb, (const uint8_t *)"lab", 3, 0) == IWM_OK);
        assert(iwm_assoc_build_assoc_req(&as) == plain + 9U);
        /* policy on: HT, then WMM */
        setup(&a, &as, &m);
        as.ht = 1;
        assert(iwm_assoc_start(&as, &hb, (const uint8_t *)"lab", 3, 0) == IWM_OK);
        l = iwm_assoc_build_assoc_req(&as);
        assert(l == plain + 28U + 9U);
        assert(memcmp(as.frame + plain, ht20, 28) == 0);
        assert(memcmp(as.frame + plain + 28, wmm, 9) == 0);
        /* no 40 MHz item: channel width set (bit 1), SGI-40 (bit 6), A-MSDU
         * 7935 (bit 11) and DSSS/CCK in 40 MHz (bit 12) are all clear */
        assert((iwm_get_le16(as.frame + plain + 2) & 0x1842U) == 0);
        /* the AP has no HT element, or a short one: not announced */
        hb.ies[12] = 61;
        setup(&a, &as, &m);
        as.ht = 1;
        assert(iwm_assoc_start(&as, &hb, (const uint8_t *)"lab", 3, 0) == IWM_OK);
        assert(iwm_assoc_build_assoc_req(&as) == plain + 9U);
        hb.ies[12] = 45; hb.ies[13] = 25;
        hb.ie_length = 12 + 27 + 9;
        memcpy(hb.ies + 39, wmm, 9);
        setup(&a, &as, &m);
        as.ht = 1;
        assert(iwm_assoc_start(&as, &hb, (const uint8_t *)"lab", 3, 0) == IWM_OK);
        assert(iwm_assoc_build_assoc_req(&as) == plain + 9U);
    }

    /* ---- WMM / HT known from the parse-time flags although the cached IEs do
     * not hold the elements (w3 log: request of 66 bytes, no WMM element, to an
     * HT/VHT/HE AP whose beacon IEs were cut at the copy limit). */
    {
        static const uint8_t wmm9[9] = { 221, 7, 0x00, 0x50, 0xf2, 2, 0, 1, 0 };
        struct iwm_scan_result fb = make_bss(AP1, "lab", -50, 0, 0, 0x0421);
        uint16_t plain, l;
        setup(&a, &as, &m);
        assert(iwm_assoc_start(&as, &fb, (const uint8_t *)"lab", 3, 0) == IWM_OK);
        plain = iwm_assoc_build_assoc_req(&as);
        fb.wmm = 1;
        setup(&a, &as, &m);
        assert(iwm_assoc_start(&as, &fb, (const uint8_t *)"lab", 3, 0) == IWM_OK);
        assert(iwm_assoc_build_assoc_req(&as) == plain + 9U);
        assert(memcmp(as.frame + plain, wmm9, 9) == 0);
        fb.ht = 1;                                    /* policy off: no HT element */
        setup(&a, &as, &m);
        assert(iwm_assoc_start(&as, &fb, (const uint8_t *)"lab", 3, 0) == IWM_OK);
        assert(iwm_assoc_build_assoc_req(&as) == plain + 9U);
        setup(&a, &as, &m);                           /* policy on: HT, then WMM */
        as.ht = 1;
        assert(iwm_assoc_start(&as, &fb, (const uint8_t *)"lab", 3, 0) == IWM_OK);
        l = iwm_assoc_build_assoc_req(&as);
        assert(l == plain + 28U + 9U && as.frame[plain] == 45 &&
               memcmp(as.frame + plain + 28, wmm9, 9) == 0);
        fb.ht = 0;                                    /* HT policy needs an HT AP */
        setup(&a, &as, &m);
        as.ht = 1;
        assert(iwm_assoc_start(&as, &fb, (const uint8_t *)"lab", 3, 0) == IWM_OK);
        assert(iwm_assoc_build_assoc_req(&as) == plain + 9U);
    }

    /* ---- ADDBA Request from the AP is declined with a Block Ack action frame
     * (the d26 Linux capture shows the accepting response 24 ms after the
     * association request: 03 01 <dialog> 00 00 <params> <timeout>). */
    {
        static uint8_t req[40], rsp[48];
        uint16_t l, fl;
        uint8_t f0;
        setup(&a, &as, &m);
        bss = make_bss(AP1, "lab", -50, 0, 0, 0x0421);
        assert(iwm_assoc_start(&as, &bss, (const uint8_t *)"lab", 3, 0) == IWM_OK);
        memset(req, 0, sizeof(req));
        req[0] = 0xd0;
        memcpy(req + 4, OWN, 6);
        memcpy(req + 10, AP1, 6);
        memcpy(req + 16, AP1, 6);
        req[24] = 3; req[25] = 0; req[26] = 0xaa;     /* ADDBA Request, dialog */
        req[27] = 0x03; req[28] = 0x10;               /* TID 0, 64 buffers */
        req[29] = 0x34; req[30] = 0x12;               /* timeout */
        req[31] = 0xc0; req[32] = 0x04;               /* starting sequence */
        assert(iwm_assoc_build_addba_decline(&as, req, 33, rsp, sizeof(rsp)) == 0);
        len = mk(f, IWM_FC0_AUTH, OWN, AP1, 0, 2, 0);
        iwm_assoc_rx(&as, f, (uint16_t)len, 1);
        len = mk(f, IWM_FC0_ASSOC_RESP, OWN, AP1, 1, 0, 0xc001);
        iwm_assoc_rx(&as, f, (uint16_t)len, 2);
        assert(as.state == IWM_AS_UP);
        fl = as.frame_len;
        f0 = as.frame[0];
        l = iwm_assoc_build_addba_decline(&as, req, 33, rsp, sizeof(rsp));
        assert(as.frame_len == fl && as.frame[0] == f0);   /* as->frame untouched */
        assert(l == 33);
        assert(rsp[0] == 0xd0 && rsp[1] == 0 && rsp[2] == 0 && rsp[3] == 0);
        assert(memcmp(rsp + 4, AP1, 6) == 0 && memcmp(rsp + 10, OWN, 6) == 0 &&
               memcmp(rsp + 16, AP1, 6) == 0);
        assert(rsp[24] == 3 && rsp[25] == 1 && rsp[26] == 0xaa);
        assert(rsp[27] == 37 && rsp[28] == 0);        /* request declined */
        assert(rsp[29] == 0x03 && rsp[30] == 0x10 && rsp[31] == 0x34 &&
               rsp[32] == 0x12);
        /* Not an ADDBA request from our BSS to this station. */
        req[25] = 1;                                  /* ADDBA Response */
        assert(iwm_assoc_build_addba_decline(&as, req, 33, rsp, sizeof(rsp)) == 0);
        req[25] = 0; req[24] = 4;                     /* other category */
        assert(iwm_assoc_build_addba_decline(&as, req, 33, rsp, sizeof(rsp)) == 0);
        req[24] = 3; req[0] = 0xe0;                   /* action no ack */
        assert(iwm_assoc_build_addba_decline(&as, req, 33, rsp, sizeof(rsp)) == 0);
        req[0] = 0xd0; req[1] = IWM_FC1_PROTECTED;
        assert(iwm_assoc_build_addba_decline(&as, req, 33, rsp, sizeof(rsp)) == 0);
        req[1] = 0; req[4] ^= 1;                      /* not addressed to us */
        assert(iwm_assoc_build_addba_decline(&as, req, 33, rsp, sizeof(rsp)) == 0);
        req[4] ^= 1; req[10] ^= 1;                    /* not from our BSS */
        assert(iwm_assoc_build_addba_decline(&as, req, 33, rsp, sizeof(rsp)) == 0);
        req[10] ^= 1;
        assert(iwm_assoc_build_addba_decline(&as, req, 32, rsp, sizeof(rsp)) == 0);
        assert(iwm_assoc_build_addba_decline(&as, req, 33, rsp, 32) == 0);
        assert(iwm_assoc_build_addba_decline(&as, req, 33, rsp, sizeof(rsp)) == 33);
        iwm_assoc_stop(&as);                          /* no longer UP */
        assert(iwm_assoc_build_addba_decline(&as, req, 33, rsp, sizeof(rsp)) == 0);
    }

    /* ---- Full exchange. */
    setup(&a, &as, &m);
    bss = make_bss(AP1, "lab", -50, 0, 0, 0x0421);
    assert(iwm_assoc_start(&as, &bss, (const uint8_t *)"lab", 3, 0) == IWM_OK);
    assert(m.prepares == 1 && m.txs == 1 && m.fc0[0] == 0xb0);
    len = mk(f, IWM_FC0_AUTH, OWN, AP1, 0, 2, 0);
    iwm_assoc_rx(&as, f, (uint16_t)len, 5);
    assert(as.state == IWM_AS_ASSOC && m.txs == 2 && m.fc0[1] == 0x00);
    assert(as.tries == 1 && as.tx_assoc == 1 && as.deadline_ms == 505);
    len = mk(f, IWM_FC0_ASSOC_RESP, OWN, AP1, 0x0421, 0, 0xc001);
    iwm_assoc_rx(&as, f, (uint16_t)len, 9);
    assert(as.state == IWM_AS_UP && as.aid == 1 && m.ups == 1 && m.aid == 1);
    assert(as.fail == IWM_AF_NONE);
    /* Disassociation by the AP after being up. */
    len = mk(f, IWM_FC0_DISASSOC, OWN, AP1, 8, 0, 0);
    iwm_assoc_rx(&as, f, 26, 20);
    assert(as.state == IWM_AS_FAILED && as.fail == IWM_AF_DEAUTH &&
           as.status == 8 && m.downs == 1);
    /* Local stop from UP sends a deauthentication and tears down. */
    setup(&a, &as, &m);
    assert(iwm_assoc_start(&as, &bss, (const uint8_t *)"lab", 3, 0) == IWM_OK);
    len = mk(f, IWM_FC0_AUTH, OWN, AP1, 0, 2, 0);
    iwm_assoc_rx(&as, f, (uint16_t)len, 1);
    len = mk(f, IWM_FC0_ASSOC_RESP, OWN, AP1, 1, 0, 2);
    iwm_assoc_rx(&as, f, (uint16_t)len, 2);
    assert(as.state == IWM_AS_UP);
    iwm_assoc_stop(&as);
    assert(as.state == IWM_AS_IDLE && m.downs == 1 && m.fc0[2] == 0xc0 &&
           m.last_len == 26 && m.last[24] == 3);

    /* ---- Frames that must be ignored leave the state unchanged. */
    setup(&a, &as, &m);
    assert(iwm_assoc_start(&as, &bss, (const uint8_t *)"lab", 3, 0) == IWM_OK);
    len = mk(f, IWM_FC0_AUTH, OWN, OTHER, 0, 2, 0);              /* wrong BSS */
    iwm_assoc_rx(&as, f, (uint16_t)len, 1);
    len = mk(f, IWM_FC0_AUTH, OTHER, AP1, 0, 2, 0);              /* not to us */
    iwm_assoc_rx(&as, f, (uint16_t)len, 1);
    len = mk(f, IWM_FC0_AUTH, OWN, AP1, 0, 2, 0);
    f[1] = IWM_FC1_PROTECTED;
    iwm_assoc_rx(&as, f, (uint16_t)len, 1);
    len = mk(f, IWM_FC0_AUTH, OWN, AP1, 1, 2, 0);                /* wrong algo */
    iwm_assoc_rx(&as, f, (uint16_t)len, 1);
    len = mk(f, IWM_FC0_AUTH, OWN, AP1, 0, 3, 0);                /* wrong seq */
    iwm_assoc_rx(&as, f, (uint16_t)len, 1);
    len = mk(f, IWM_FC0_AUTH, OWN, AP1, 0, 2, 0);
    iwm_assoc_rx(&as, f, 29, 1);                                 /* short */
    len = mk(f, IWM_FC0_ASSOC_RESP, OWN, AP1, 1, 0, 2);          /* too early */
    iwm_assoc_rx(&as, f, (uint16_t)len, 1);
    assert(as.state == IWM_AS_AUTH && as.rx_ignored == 7U && m.txs == 1);
    /* Broadcast deauthentication is accepted. */
    memset(f, 0xff, 32);
    mk(f, IWM_FC0_DEAUTH, (const uint8_t *)"\xff\xff\xff\xff\xff\xff", AP1, 6, 0, 0);
    iwm_assoc_rx(&as, f, 26, 1);
    assert(as.state == IWM_AS_FAILED && as.fail == IWM_AF_DEAUTH && as.status == 6);

    /* ---- Refusals carry the AP's status code. */
    setup(&a, &as, &m);
    assert(iwm_assoc_start(&as, &bss, (const uint8_t *)"lab", 3, 0) == IWM_OK);
    len = mk(f, IWM_FC0_AUTH, OWN, AP1, 0, 2, 17);
    iwm_assoc_rx(&as, f, (uint16_t)len, 1);
    assert(as.state == IWM_AS_FAILED && as.fail == IWM_AF_AUTH_REFUSED &&
           as.status == 17 && m.txs == 1);
    setup(&a, &as, &m);
    assert(iwm_assoc_start(&as, &bss, (const uint8_t *)"lab", 3, 0) == IWM_OK);
    len = mk(f, IWM_FC0_AUTH, OWN, AP1, 0, 2, 0);
    iwm_assoc_rx(&as, f, (uint16_t)len, 1);
    len = mk(f, IWM_FC0_ASSOC_RESP, OWN, AP1, 1, 18, 0);
    iwm_assoc_rx(&as, f, (uint16_t)len, 2);
    assert(as.state == IWM_AS_FAILED && as.fail == IWM_AF_ASSOC_REFUSED &&
           as.status == 18 && m.ups == 0);
    iwm_assoc_rx(&as, f, (uint16_t)len, 3);                      /* after failure */
    assert(as.state == IWM_AS_FAILED);

    /* ---- Timeouts: three transmissions, then failure. */
    setup(&a, &as, &m);
    assert(iwm_assoc_start(&as, &bss, (const uint8_t *)"lab", 3, 100) == IWM_OK);
    iwm_assoc_tick(&as, 399);
    assert(m.txs == 1);
    iwm_assoc_tick(&as, 400);
    assert(m.txs == 2 && as.tries == 2 && as.deadline_ms == 700);
    iwm_assoc_tick(&as, 700);
    assert(m.txs == 3 && as.tries == 3);
    iwm_assoc_tick(&as, 1000);
    assert(as.state == IWM_AS_FAILED && as.fail == IWM_AF_AUTH_TIMEOUT &&
           m.txs == 3);
    setup(&a, &as, &m);
    assert(iwm_assoc_start(&as, &bss, (const uint8_t *)"lab", 3, 0) == IWM_OK);
    len = mk(f, IWM_FC0_AUTH, OWN, AP1, 0, 2, 0);
    iwm_assoc_rx(&as, f, (uint16_t)len, 10);
    for (now = 510, i = 0; i < 2; ++i, now += 500)
        iwm_assoc_tick(&as, now);
    assert(as.state == IWM_AS_ASSOC && as.tx_assoc == 3);
    iwm_assoc_tick(&as, now);
    assert(as.state == IWM_AS_FAILED && as.fail == IWM_AF_ASSOC_TIMEOUT);

    /* ---- Start-up refusals. */
    setup(&a, &as, &m);
    m.tx_rc = IWM_ENOTSUP;
    assert(iwm_assoc_start(&as, &bss, (const uint8_t *)"lab", 3, 0) == IWM_ENOTSUP);
    assert(as.state == IWM_AS_FAILED && as.fail == IWM_AF_NO_TX && m.prepares == 1);
    setup(&a, &as, &m);
    m.tx_rc = IWM_EIO;
    assert(iwm_assoc_start(&as, &bss, (const uint8_t *)"lab", 3, 0) == IWM_EIO);
    assert(as.fail == IWM_AF_TX);
    setup(&a, &as, &m);
    m.prepare_rc = IWM_EIO;
    assert(iwm_assoc_start(&as, &bss, (const uint8_t *)"lab", 3, 0) == IWM_EIO);
    assert(as.fail == IWM_AF_PREPARE && m.txs == 0);
    setup(&a, &as, &m);
    bss = make_bss(AP2, "wpa", -40, 1, 0, 0x0011);
    assert(iwm_assoc_start(&as, &bss, (const uint8_t *)"wpa", 3, 0) == IWM_ENOTSUP);
    assert(as.fail == IWM_AF_PRIVACY && m.prepares == 0 && m.txs == 0);
    bss = make_bss(AP1, "lab", -50, 0, 0, 0x0001);
    assert(iwm_assoc_start(&as, &bss, (const uint8_t *)"lab", 0, 0) == IWM_EINVAL);
    assert(iwm_assoc_start(&as, &bss, (const uint8_t *)"lab", 33, 0) == IWM_EINVAL);
    assert(iwm_assoc_start(&as, NULL, (const uint8_t *)"lab", 3, 0) == IWM_EINVAL);
    a.mac_valid = 0;
    assert(iwm_assoc_start(&as, &bss, (const uint8_t *)"lab", 3, 0) == IWM_ESTATE);
    /* No ops at all behaves as "no TX path". */
    iwm_adapter_init(&a, NULL, NULL);
    assert(iwm_adapter_set_mac(&a, OWN) == IWM_OK);
    iwm_assoc_init(&as, &a, NULL, NULL);
    assert(iwm_assoc_start(&as, &bss, (const uint8_t *)"lab", 3, 0) == IWM_ENOTSUP);
    assert(as.fail == IWM_AF_NO_TX);

    /* ---- RX descriptor helper feeds the same frame to the state machine. */
    {
        const uint8_t *fr;
        uint32_t fl;
        len = tb_build(buf, IWM_80211_SUBTYPE_BEACON, AP1, "lab", 1, 1, 0x40,
                       1, 0x0003);
        assert(iwm_rx_mpdu_frame(buf, len, &fr, &fl) == IWM_OK);
        assert(fr == buf + IWM_RX_MPDU_DESC_SIZE && fr[0] == 0x80 &&
               fl == len - IWM_RX_MPDU_DESC_SIZE - 6U);
        len = tb_build(buf, IWM_80211_SUBTYPE_BEACON, AP1, "lab", 1, 1, 0x40,
                       1, 0x0002);
        assert(iwm_rx_mpdu_frame(buf, len, &fr, &fl) == IWM_EIO);
        assert(iwm_rx_mpdu_frame(buf, 48U + 23U, &fr, &fl) == IWM_EFORMAT);
        assert(iwm_rx_mpdu_frame(NULL, 100, &fr, &fl) == IWM_EINVAL);
    }

    /* ---- GenMAC: requests are recorded for the OS layer. */
    {
        static struct iwm_ndis_ssid ns;
        iwm_adapter_init(&a, NULL, NULL);
        ns.length = 3;
        memcpy(ns.ssid, "lab", 3);
        oid_set(&a, OID_802_11_SSID, &ns, sizeof(ns), IWM_OK);
        assert(a.assoc_request == 0 && a.ssid_length == 3);   /* fw not up */
        a.firmware.state = IWM_FW_RUNNING;
        oid_set(&a, OID_802_11_SSID, &ns, sizeof(ns), IWM_OK);
        assert(a.assoc_request == 1);
        a.assoc_request = 0;
        /* Not associated: GET reports an empty SSID, the request is kept. */
        {
            static struct iwm_ndis_ssid got;
            struct iwm_oid_request gr;
            memset(&gr, 0, sizeof(gr));
            memset(&got, 0xee, sizeof(got));
            gr.oid = OID_802_11_SSID;
            gr.data = &got;
            gr.data_length = sizeof(got);
            assert(iwm_genmac_oid_get(&a, &gr) == IWM_OK);
            assert(gr.bytes_done == sizeof(got) && got.length == 0 &&
                   got.ssid[0] == 0 && a.ssid_length == 3);
            a.connected = 1;
            memset(&got, 0, sizeof(got));
            assert(iwm_genmac_oid_get(&a, &gr) == IWM_OK);
            assert(got.length == 3 && memcmp(got.ssid, "lab", 3) == 0);
            a.connected = 0;
        }
        ns.length = 0;
        oid_set(&a, OID_802_11_SSID, &ns, sizeof(ns), IWM_OK);
        assert(a.assoc_request == 0 && a.ssid_length == 0);
        oid_set(&a, OID_802_11_BSSID, (void *)AP1, 6, IWM_OK);
        assert(a.req_bssid_valid && memcmp(a.req_bssid, AP1, 6) == 0);
        oid_set(&a, OID_802_11_BSSID, (void *)"\xff\xff\xff\xff\xff\xff", 6, IWM_OK);
        assert(!a.req_bssid_valid);
        oid_set(&a, OID_802_11_BSSID, (void *)AP1, 5, IWM_EINVAL);
        /* Link state reported to XWLAN. */
        a.state = IWM_ADAPTER_READY;
        rc = (int)a.status_generation;
        iwm_genmac_link_up(&a, AP1, -50);
        assert(a.connected && a.state == IWM_ADAPTER_ASSOCIATED &&
               memcmp(a.bssid, AP1, 6) == 0 && a.rssi_dbm == -50 &&
               (int)a.status_generation == rc + 1);
        oid_set(&a, OID_802_11_DISASSOCIATE, &ns, 4, IWM_OK);
        assert(a.assoc_request == 2 && !a.connected &&
               a.state == IWM_ADAPTER_READY);
        iwm_genmac_link_up(&a, AP1, -50);
        iwm_genmac_link_down(&a);
        assert(!a.connected && a.state == IWM_ADAPTER_READY &&
               a.bssid[0] == 0);
    }

    puts("association tests passed");
    return 0;
}
