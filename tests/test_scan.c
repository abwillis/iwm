/* SPDX-License-Identifier: GPL-2.0-only */
/* RX MPDU beacon parser and scan-cache upsert. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "iwm_test_beacon.h"

static uint32_t regs[64];
static uint32_t rd(void *c, uint32_t o) { (void)c; return regs[(o >> 2) & 63]; }
static void wr(void *c, uint32_t o, uint32_t v) { (void)c; regs[(o >> 2) & 63] = v; }
static void dl(void *c, uint32_t u) { (void)c; (void)u; }

/* First 64 payload bytes of "mpdu 0" in the 9560 WRND32$ log. */
static const uint8_t logged[64] = {
    0x06, 0x01, 0x02, 0x0c, 0x00, 0x00, 0x02, 0x34,
    0xc3, 0x7e, 0x3f, 0x00, 0x1b, 0x10, 0xc0, 0xc0,
    0x00, 0x80, 0x68, 0x7f, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x05, 0x40, 0x00, 0x00,
    0x39, 0x7f, 0x01, 0x00, 0xaa, 0x80, 0x00, 0x00,
    0xa9, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xf0, 0x1d, 0x2d, 0x92, 0x32, 0xe5
};

int main(void)
{
    static uint8_t buf[1024];
    static struct iwm_scan_result r;
    static struct iwm_scan_cache cache;
    static const uint8_t bss1[6] = { 0x00, 0x16, 0x3e, 0x12, 0x34, 0x56 };
    static const uint8_t bss2[6] = { 0x02, 0x16, 0x3e, 0x12, 0x34, 0x57 };
    struct iwm_adapter a;
    struct iwm_os_ops ops;
    uint32_t len, pos, i;

    /* Real descriptor and header from the card, body synthesized to the
     * logged mpdu_len (262): SSID "test", rates, DS 1, then a vendor IE. */
    memset(buf, 0, sizeof(buf));
    memcpy(buf, logged, sizeof(logged));
    memcpy(buf + 64, buf + 58, 6);        /* addr3 = addr2 */
    pos = 48U + 24U;
    buf[pos + 8] = 100;                   /* interval */
    buf[pos + 10] = 0x11;                 /* ESS | privacy */
    pos += 12U;
    buf[pos++] = 0; buf[pos++] = 4; memcpy(buf + pos, "test", 4); pos += 4;
    buf[pos++] = 1; buf[pos++] = 8;
    memcpy(buf + pos, "\x82\x84\x8b\x96\x0c\x12\x18\x24", 8); pos += 8;
    buf[pos++] = 3; buf[pos++] = 1; buf[pos++] = 1;
    buf[pos++] = 221; buf[pos++] = 205; pos += 205;
    assert(pos == 48U + 262U);
    assert(iwm_scan_parse_mpdu(buf, 316, &r) == IWM_OK);
    assert(memcmp(r.bssid, "\xf0\x1d\x2d\x92\x32\xe5", 6) == 0);
    assert(r.rssi_dbm == -57);
    assert(r.ssid_length == 4 && memcmp(r.ssid, "test", 4) == 0);
    assert(r.privacy == 1 && r.infrastructure_mode == IWM_INFRASTRUCTURE_BSS);
    assert(r.ds_config_khz == 2412000UL);
    assert(r.network_type == IWM_NETWORK_TYPE_OFDM24);
    assert(r.beacon_period == 100);
    assert(r.rates[0] == 0x82 && r.rates[7] == 0x24 && r.rates[8] == 0);
    assert(r.ie_length == 238U);          /* fixed 12 + all IEs */
    assert(r.ies[8] == 100 && r.ies[12] == 0 && r.ies[13] == 4);

    /* Probe response, 5 GHz from the descriptor, hidden SSID, no privacy. */
    len = tb_build(buf, IWM_80211_SUBTYPE_PROBE_RESP, bss1, "", 36, 0,
                   0x48, IWM_80211_CAP_ESS, 0x0003);
    assert(iwm_scan_parse_mpdu(buf, len, &r) == IWM_OK);
    assert(r.ssid_length == 0 && r.privacy == 0);
    assert(r.ds_config_khz == 5180000UL);
    assert(r.network_type == IWM_NETWORK_TYPE_OFDM5);
    assert(r.rssi_dbm == -72);
    assert(r.rates[8] == 0x30 && r.rates[11] == 0x6c);

    /* DS parameter set wins over the descriptor channel; IBSS. */
    len = tb_build(buf, IWM_80211_SUBTYPE_BEACON, bss1, "adhoc", 1, 6,
                   0, IWM_80211_CAP_IBSS, 0x0003);
    assert(iwm_scan_parse_mpdu(buf, len, &r) == IWM_OK);
    assert(r.ds_config_khz == 2437000UL);
    assert(r.infrastructure_mode == IWM_INFRASTRUCTURE_IBSS);
    assert(r.rssi_dbm == -127);           /* chain A empty, chain B 0x7f */

    /* Not a beacon / probe response; bad CRC; truncations. */
    len = tb_build(buf, 4, bss1, "x", 1, 1, 0x40, 1, 0x0003);
    assert(iwm_scan_parse_mpdu(buf, len, &r) == IWM_ENOTSUP);
    buf[48] = 0x08;                       /* data frame */
    assert(iwm_scan_parse_mpdu(buf, len, &r) == IWM_ENOTSUP);
    len = tb_build(buf, IWM_80211_SUBTYPE_BEACON, bss1, "x", 1, 1, 0x40, 1,
                   0x0002);
    assert(iwm_scan_parse_mpdu(buf, len, &r) == IWM_EIO);
    len = tb_build(buf, IWM_80211_SUBTYPE_BEACON, bss1, "x", 1, 1, 0x40, 1,
                   0x0003);
    assert(iwm_scan_parse_mpdu(buf, 48U + 23U, &r) == IWM_EFORMAT);
    assert(iwm_scan_parse_mpdu(buf, len - 7U, &r) == IWM_EFORMAT);
    buf[0] = 30;                          /* header + 6: fixed fields cut */
    buf[1] = 0;
    assert(iwm_scan_parse_mpdu(buf, len, &r) == IWM_EFORMAT);

    /* An IE running past the body is ignored; earlier IEs survive. */
    len = tb_build(buf, IWM_80211_SUBTYPE_BEACON, bss1, "abc", 1, 0, 0x40, 1,
                   0x0003);
    buf[48U + 24U + 12U + 5U + 1U] = 200; /* rates IE claims 200 bytes */
    assert(iwm_scan_parse_mpdu(buf, len, &r) == IWM_OK);
    assert(r.ssid_length == 3 && r.rates[0] == 0);
    assert(r.ie_length == 12U + 5U);

    /* IE copy stops at a whole-IE boundary within IWM_SCAN_MAX_IE. */
    memset(buf, 0, sizeof(buf));
    len = tb_build(buf, IWM_80211_SUBTYPE_BEACON, bss1, "big", 1, 1, 0x40, 1,
                   0x0003);
    pos = 48U + ((uint32_t)buf[0] | ((uint32_t)buf[1] << 8));
    for (i = 0; i < 3; ++i) {
        buf[pos++] = 221; buf[pos++] = 100; pos += 100;
    }
    buf[0] = (uint8_t)(pos - 48U);
    buf[1] = (uint8_t)((pos - 48U) >> 8);
    assert(iwm_scan_parse_mpdu(buf, pos + 6U, &r) == IWM_OK);
    assert(r.ie_length == 12U + 5U + 10U + 3U + 6U + 102U * 2U);
    assert(r.ie_length <= IWM_SCAN_MAX_IE);

    /* WMM and HT elements that lie beyond the IE copy limit are still reported
     * (w2/w3 log: beacons of 334-367 IE bytes, 185-250 cached).  Two 100-byte
     * vendor elements nearly fill the copy; HT capabilities and WMM follow. */
    memset(buf, 0, sizeof(buf));
    len = tb_build(buf, IWM_80211_SUBTYPE_BEACON, bss1, "late", 1, 1, 0x40, 1,
                   0x0003);
    pos = 48U + ((uint32_t)buf[0] | ((uint32_t)buf[1] << 8));
    for (i = 0; i < 2; ++i) {
        buf[pos++] = 221; buf[pos++] = 100; buf[pos + 0] = 0x00; buf[pos + 1] = 0x11;
        buf[pos + 2] = 0x22; buf[pos + 3] = 0x01; pos += 100;
    }
    buf[pos++] = 45; buf[pos++] = 26; pos += 26;                    /* HT caps */
    buf[pos++] = 221; buf[pos++] = 24;                             /* WMM parameter */
    buf[pos++] = 0x00; buf[pos++] = 0x50; buf[pos++] = 0xf2; buf[pos++] = 2;
    buf[pos++] = 1; pos += 19;
    buf[0] = (uint8_t)(pos - 48U);
    buf[1] = (uint8_t)((pos - 48U) >> 8);
    assert(iwm_scan_parse_mpdu(buf, pos + 6U, &r) == IWM_OK);
    assert(r.ie_length < r.ie_total + IWM_80211_FIXED_LEN);          /* copy was cut */
    assert(r.ie_total == 6U + 10U + 3U + 6U + 102U * 2U + 28U + 26U);
    assert(r.wmm == 1 && r.ht == 1);
    {
        uint32_t q = 12U;                    /* the HT element is not in the copy */
        while (q + 2U <= r.ie_length) {
            assert(r.ies[q] != 45U);
            q += 2U + r.ies[q + 1U];
        }
    }
    /* Absent, too short, wrong OUI/type: not flagged.  A WMM information
     * element (subtype 0) counts, a different vendor type does not. */
    len = tb_build(buf, IWM_80211_SUBTYPE_BEACON, bss1, "none", 1, 1, 0x40, 1,
                   0x0003);
    assert(iwm_scan_parse_mpdu(buf, len, &r) == IWM_OK);
    assert(r.wmm == 0 && r.ht == 0 && r.ie_total == 6U + 10U + 3U + 6U);
    pos = 48U + ((uint32_t)buf[0] | ((uint32_t)buf[1] << 8));
    buf[pos++] = 45; buf[pos++] = 25; pos += 25;                    /* short HT */
    buf[pos++] = 221; buf[pos++] = 7;                              /* WMM info */
    buf[pos++] = 0x00; buf[pos++] = 0x50; buf[pos++] = 0xf2; buf[pos++] = 2;
    buf[pos++] = 0; buf[pos++] = 1; buf[pos++] = 0;
    buf[0] = (uint8_t)(pos - 48U);
    buf[1] = (uint8_t)((pos - 48U) >> 8);
    assert(iwm_scan_parse_mpdu(buf, pos + 6U, &r) == IWM_OK);
    assert(r.wmm == 1 && r.ht == 0);
    buf[pos - 5U] = 4;                                              /* vendor type 4 */
    assert(iwm_scan_parse_mpdu(buf, pos + 6U, &r) == IWM_OK && r.wmm == 0);

    /* Beacon TIM: AID bit, group-buffered bit, bitmap offset, bounds. */
    {
        static uint8_t fr[160];
        struct iwm_tim_info ti;
        uint32_t n;
        memset(fr, 0, sizeof(fr));
        fr[0] = 0x80;
        n = IWM_80211_HDR_LEN + IWM_80211_FIXED_LEN;
        fr[n++] = 0; fr[n++] = 3; memcpy(fr + n, "tim", 3); n += 3;   /* SSID */
        fr[n++] = 5; fr[n++] = 6;                                      /* TIM */
        fr[n++] = 2;           /* dtim count */
        fr[n++] = 3;           /* dtim period */
        fr[n++] = 0x01;        /* control: group buffered, offset 0 */
        fr[n++] = 0x02;        /* AID 1 */
        fr[n++] = 0x00;
        fr[n++] = 0x80;        /* AID 23 */
        assert(iwm_beacon_tim(fr, n, 1, &ti) == IWM_OK);
        assert(ti.present && ti.dtim_count == 2 && ti.dtim_period == 3 &&
               ti.group_set == 1 && ti.aid_set == 1 && ti.bitmap_ctl == 1);
        assert(iwm_beacon_tim(fr, n, 2, &ti) == IWM_OK && !ti.aid_set);
        assert(iwm_beacon_tim(fr, n, 23, &ti) == IWM_OK && ti.aid_set);
        assert(iwm_beacon_tim(fr, n, 24, &ti) == IWM_OK && !ti.aid_set);
        assert(iwm_beacon_tim(fr, n, 2007, &ti) == IWM_OK && !ti.aid_set);
        /* A bitmap that starts at octet 2 (control 0x02) covers AIDs 16 and up
         * only: its first octet 0x02 is AID 17, the last one 0x80 is AID 39. */
        fr[IWM_80211_HDR_LEN + IWM_80211_FIXED_LEN + 5U + 4U] = 0x02;
        assert(iwm_beacon_tim(fr, n, 1, &ti) == IWM_OK && ti.present &&
               !ti.aid_set && ti.group_set == 0);
        assert(iwm_beacon_tim(fr, n, 16, &ti) == IWM_OK && !ti.aid_set);
        assert(iwm_beacon_tim(fr, n, 17, &ti) == IWM_OK && ti.aid_set);
        assert(iwm_beacon_tim(fr, n, 39, &ti) == IWM_OK && ti.aid_set);
        assert(iwm_beacon_tim(fr, n, 40, &ti) == IWM_OK && !ti.aid_set);
        /* No TIM, truncated element, not a beacon, too short. */
        fr[IWM_80211_HDR_LEN + IWM_80211_FIXED_LEN + 5U] = 7;
        assert(iwm_beacon_tim(fr, n, 1, &ti) == IWM_OK && !ti.present);
        assert(iwm_beacon_tim(fr, IWM_80211_HDR_LEN + IWM_80211_FIXED_LEN, 1, &ti) ==
               IWM_OK && !ti.present);
        assert(iwm_beacon_tim(fr, IWM_80211_HDR_LEN + IWM_80211_FIXED_LEN - 1U, 1,
                              &ti) == IWM_EFORMAT);
        fr[0] = 0x50;
        assert(iwm_beacon_tim(fr, n, 1, &ti) == IWM_ENOTSUP);
        assert(iwm_beacon_tim(NULL, n, 1, &ti) == IWM_EINVAL &&
               iwm_beacon_tim(fr, n, 1, NULL) == IWM_EINVAL);
    }

    /* Cache: insert, replace by BSSID, separate BSS, overflow. */
    memset(&ops, 0, sizeof(ops));
    ops.mmio_read32 = rd;
    ops.mmio_write32 = wr;
    ops.delay_us = dl;
    iwm_adapter_init(&a, &ops, NULL);
    assert(iwm_genmac_scan_upsert(&a, &r) == IWM_EINVAL);
    iwm_genmac_bind_scan_cache(&a, &cache);
    len = tb_build(buf, IWM_80211_SUBTYPE_BEACON, bss1, "one", 1, 1, 0x40, 1,
                   0x0003);
    assert(iwm_scan_parse_mpdu(buf, len, &r) == IWM_OK);
    assert(iwm_genmac_scan_upsert(&a, &r) == IWM_OK && cache.count == 1);
    r.rssi_dbm = -40;
    assert(iwm_genmac_scan_upsert(&a, &r) == IWM_OK && cache.count == 1);
    assert(cache.result[0].rssi_dbm == -40);
    len = tb_build(buf, IWM_80211_SUBTYPE_BEACON, bss2, "two", 6, 6, 0x50, 1,
                   0x0003);
    assert(iwm_scan_parse_mpdu(buf, len, &r) == IWM_OK);
    assert(iwm_genmac_scan_upsert(&a, &r) == IWM_OK && cache.count == 2);
    for (i = 2; i < IWM_SCAN_MAX_RESULTS; ++i) {
        r.bssid[5] = (uint8_t)(0x80 + i);
        assert(iwm_genmac_scan_upsert(&a, &r) == IWM_OK);
    }
    r.bssid[5] = 0xff;
    assert(iwm_genmac_scan_upsert(&a, &r) == IWM_EOVERFLOW);
    r.bssid[5] = 0x57;                    /* existing entry still updatable */
    assert(iwm_genmac_scan_upsert(&a, &r) == IWM_OK);
    assert(cache.count == IWM_SCAN_MAX_RESULTS);

    puts("scan parser tests passed");
    return 0;
}
