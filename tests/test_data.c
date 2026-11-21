/* SPDX-License-Identifier: GPL-2.0-only */
/* Data path: 802.3 <-> 802.11 conversion, DHCP probe, scan while connected. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "iwm_data.h"
#include "iwm_genmac.h"

static const uint8_t OWN[6] = { 0xd4, 0x3b, 0x04, 0xc3, 0x04, 0x74 };
static const uint8_t AP[6]  = { 0x80, 0xaf, 0x9f, 0x80, 0xf1, 0x8f };
static const uint8_t GW[6]  = { 0x80, 0xaf, 0x9f, 0x80, 0xf1, 0x80 };
static const uint8_t BC[6]  = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/* RX MPDU payload: 48-byte descriptor (mpdu_len, mac_flags2, status CRC+
 * overrun OK) then the 802.11 frame. */
static uint32_t mk_rx(uint8_t *p, uint8_t fc0, uint8_t fc1, const uint8_t *a1,
                      const uint8_t *a2, const uint8_t *a3, int pad,
                      uint8_t qos0, const uint8_t *body, uint16_t blen)
{
    uint8_t *f = p + 48;
    uint32_t hdr = (fc0 & 0x80) ? 26U : 24U, n;
    memset(p, 0, 48 + 64);
    f[0] = fc0;
    f[1] = fc1;
    memcpy(f + 4, a1, 6);
    memcpy(f + 10, a2, 6);
    memcpy(f + 16, a3, 6);
    if (fc0 & 0x80)
        f[24] = qos0;
    n = hdr + (pad ? 2U : 0U);
    memcpy(f + n, body, blen);
    n += blen;
    p[0] = (uint8_t)n;
    p[1] = (uint8_t)(n >> 8);
    p[3] = (uint8_t)((hdr / 2U) | (pad ? IWM_RX_MPDU_MFLG2_PAD : 0U));
    p[12] = 0x03;
    return 48U + n + 4U;
}

/* The DHCPDISCOVER Linux (dhcpcd) sent on LWGuest in the d26 capture, as an
 * Ethernet II frame; the network answered it.  xid 7965cca7, IP id ddd9,
 * IP checksum 9bcc, UDP checksum 243a.  Everything between the first 76 bytes
 * and the magic cookie at offset 278, and the 15 bytes after the options, is
 * zero in the capture, so only the two non-zero parts are listed. */
static const uint8_t linux_head[76] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xd4, 0x3b, 0x04, 0xc3, 0x04, 0x74,
    0x08, 0x00, 0x45, 0x00, 0x01, 0x48, 0xdd, 0xd9, 0x00, 0x00, 0x40, 0x11,
    0x9b, 0xcc, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00, 0x44,
    0x00, 0x43, 0x01, 0x34, 0x24, 0x3a, 0x01, 0x01, 0x06, 0x00, 0x79, 0x65,
    0xcc, 0xa7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd4, 0x3b,
    0x04, 0xc3, 0x04, 0x74
};
static const uint8_t linux_opts[45] = {
    0x35, 0x01, 0x01, 0x37, 0x0b, 0x01, 0x79, 0x03, 0x06, 0x0c, 0x0f, 0x1a,
    0x1c, 0x21, 0x36, 0x77, 0x39, 0x02, 0x05, 0xc0, 0x3d, 0x13, 0xff, 0x04,
    0xc3, 0x04, 0x74, 0x00, 0x01, 0x00, 0x01, 0x32, 0x91, 0xe1, 0x1c, 0xd4,
    0x3b, 0x04, 0xc3, 0x04, 0x74, 0x91, 0x01, 0x01, 0xff
};
static uint8_t linux_discover[IWM_DATA_DHCP_LEN];

static void init_linux_discover(void)
{
    memset(linux_discover, 0, sizeof(linux_discover));
    memcpy(linux_discover, linux_head, sizeof(linux_head));
    linux_discover[278] = 0x63;                    /* DHCP magic cookie */
    linux_discover[279] = 0x82;
    linux_discover[280] = 0x53;
    linux_discover[281] = 0x63;
    memcpy(linux_discover + 282, linux_opts, sizeof(linux_opts));
}

int main(void)
{
    static uint8_t eth[1600], out[1600], p[2048], body[600];
    static struct iwm_data d;
    struct iwm_adapter a;
    static struct iwm_scan_cache cache;
    uint16_t n = 0, i;
    uint32_t len, sum;

    init_linux_discover();
    iwm_data_init(&d, NULL);
    /* not linked: nothing converts */
    assert(iwm_data_tx(&d, eth, 60) == IWM_EINVAL);   /* no transport */
    assert(iwm_data_rx(&d, p, 100, out, sizeof(out), &n) == IWM_ENOTSUP);
    iwm_data_link(&d, OWN, AP);
    assert(d.up && d.seq == 0);

    /* ---- TX conversion: IPv4 to the gateway */
    memcpy(eth, GW, 6);
    memcpy(eth + 6, OWN, 6);
    eth[12] = 0x08; eth[13] = 0x00;
    for (i = 0; i < 46; ++i)
        eth[14 + i] = (uint8_t)(0x40 + i);
    n = iwm_data_eth_to_80211(&d, eth, 60, out, sizeof(out));
    assert(n == 26U + 8U + 46U);
    assert(out[0] == 0x88 && out[1] == 0x01 && out[2] == 0 && out[3] == 0);
    assert(memcmp(out + 4, AP, 6) == 0 && memcmp(out + 10, OWN, 6) == 0 &&
           memcmp(out + 16, GW, 6) == 0);
    assert(out[22] == 0 && out[23] == 0 && out[24] == 0 && out[25] == 0);
    assert(memcmp(out + 26, "\xaa\xaa\x03\x00\x00\x00\x08\x00", 8) == 0);
    assert(out[34] == 0x40 && out[79] == 0x40 + 45);
    /* sequence field, IPX bridge tunnel, 802.3 length, bounds */
    d.seq = 0x123;
    eth[12] = 0x81; eth[13] = 0x37;
    n = iwm_data_eth_to_80211(&d, eth, 60, out, sizeof(out));
    assert(out[22] == 0x30 && out[23] == 0x12 && out[31] == 0xf8);
    eth[12] = 0x05; eth[13] = 0xdc;
    assert(iwm_data_eth_to_80211(&d, eth, 60, out, sizeof(out)) == 0);
    eth[12] = 0x08; eth[13] = 0x00;
    assert(iwm_data_eth_to_80211(&d, eth, 13, out, sizeof(out)) == 0);
    assert(iwm_data_eth_to_80211(&d, eth, 1515, out, sizeof(out)) == 0);
    assert(iwm_data_eth_to_80211(&d, eth, 60, out, 79) == 0);
    d.seq = 0;

    /* ---- RX: QoS data with firmware pad, unicast to us */
    memcpy(body, "\xaa\xaa\x03\x00\x00\x00\x08\x06", 8);
    for (i = 0; i < 28; ++i)
        body[8 + i] = (uint8_t)(0x10 + i);
    len = mk_rx(p, 0x88, 0x02, OWN, AP, GW, 1, 0, body, 36);
    assert(iwm_data_rx(&d, p, len, out, sizeof(out), &n) == IWM_OK);
    assert(n == 14U + 28U && memcmp(out, OWN, 6) == 0 &&
           memcmp(out + 6, GW, 6) == 0 && out[12] == 0x08 && out[13] == 0x06 &&
           out[14] == 0x10 && out[41] == 0x10 + 27 && d.rx_ok == 1);
    /* non-QoS, no pad, broadcast */
    len = mk_rx(p, 0x08, 0x02, BC, AP, GW, 0, 0, body, 36);
    assert(iwm_data_rx(&d, p, len, out, sizeof(out), &n) == IWM_OK);
    assert(memcmp(out, BC, 6) == 0 && n == 42U);
    /* drops */
    len = mk_rx(p, 0x88, 0x42, OWN, AP, GW, 1, 0, body, 36);      /* protected */
    assert(iwm_data_rx(&d, p, len, out, sizeof(out), &n) == IWM_ENOTSUP &&
           d.rx_protected == 1);
    len = mk_rx(p, 0x88, 0x02, OWN, GW, GW, 1, 0, body, 36);      /* other TA */
    assert(iwm_data_rx(&d, p, len, out, sizeof(out), &n) == IWM_ENOTSUP);
    len = mk_rx(p, 0x88, 0x01, OWN, AP, GW, 1, 0, body, 36);      /* ToDS */
    assert(iwm_data_rx(&d, p, len, out, sizeof(out), &n) == IWM_ENOTSUP);
    len = mk_rx(p, 0x88, 0x02, GW, AP, GW, 1, 0, body, 36);       /* not us */
    assert(iwm_data_rx(&d, p, len, out, sizeof(out), &n) == IWM_ENOTSUP &&
           d.rx_other_bss == 3);
    len = mk_rx(p, 0x88, 0x02, BC, AP, OWN, 1, 0, body, 36);      /* echo */
    assert(iwm_data_rx(&d, p, len, out, sizeof(out), &n) == IWM_ENOTSUP &&
           d.rx_echo == 1);
    len = mk_rx(p, 0xc8, 0x02, OWN, AP, GW, 1, 0, body, 0);       /* QoS null */
    assert(iwm_data_rx(&d, p, len, out, sizeof(out), &n) == IWM_ENOTSUP &&
           d.rx_null == 1);
    len = mk_rx(p, 0x88, 0x02, OWN, AP, GW, 1, 0x80, body, 36);   /* A-MSDU */
    assert(iwm_data_rx(&d, p, len, out, sizeof(out), &n) == IWM_ENOTSUP &&
           d.rx_amsdu == 1);
    len = mk_rx(p, 0x80, 0x00, BC, AP, AP, 0, 0, body, 36);       /* beacon */
    assert(iwm_data_rx(&d, p, len, out, sizeof(out), &n) == IWM_ENOTSUP &&
           d.rx_not_data == 1);
    body[0] = 0x42;                                               /* non-SNAP */
    len = mk_rx(p, 0x88, 0x02, OWN, AP, GW, 1, 0, body, 36);
    assert(iwm_data_rx(&d, p, len, out, sizeof(out), &n) == IWM_EFORMAT);
    body[0] = 0xaa;
    len = mk_rx(p, 0x88, 0x02, OWN, AP, GW, 1, 0, body, 36);
    p[12] = 0x02;                                                 /* CRC bad */
    assert(iwm_data_rx(&d, p, len, out, sizeof(out), &n) == IWM_EIO);
    len = mk_rx(p, 0x88, 0x02, OWN, AP, GW, 1, 0, body, 36);
    assert(iwm_data_rx(&d, p, len, out, 30, &n) == IWM_EOVERFLOW);
    iwm_data_unlink(&d);
    assert(iwm_data_rx(&d, p, len, out, sizeof(out), &n) == IWM_ENOTSUP);
    /* Delivered frames by address class; dropped frames are not counted. */
    assert(d.rx_ok == 2U && d.rx_ucast == 1U && d.rx_bcast == 1U &&
           d.rx_mcast == 0U);
    iwm_data_link(&d, OWN, AP);
    {
        static const uint8_t mc[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x01 };
        len = mk_rx(p, 0x88, 0x02, mc, AP, GW, 1, 0, body, 36);
        assert(iwm_data_rx(&d, p, len, out, sizeof(out), &n) == IWM_OK);
        assert(d.rx_ok == 3U && d.rx_ucast == 1U && d.rx_bcast == 1U &&
               d.rx_mcast == 1U);
    }
    iwm_data_unlink(&d);

    /* ---- QoS data with an HT Control field (Order bit set): the header is
     * 30 bytes, plus the firmware's 2-byte pad when mac_flags2 says so. */
    iwm_data_link(&d, OWN, AP);
    {
        static uint8_t hb[48];
        unsigned k;
        memcpy(hb, "\x11\x22\x33\x44", 4);                      /* HT Control */
        memcpy(hb + 4, "\xaa\xaa\x03\x00\x00\x00\x08\x06", 8);
        for (k = 0; k < 28U; ++k)
            hb[12 + k] = (uint8_t)(0x10 + k);
        len = mk_rx(p, 0x88, 0x82, OWN, AP, GW, 0, 0, hb, 40);
        assert(iwm_data_rx(&d, p, len, out, sizeof(out), &n) == IWM_OK);
        assert(n == 14U + 28U && out[12] == 0x08 && out[13] == 0x06 &&
               out[14] == 0x10 && out[41] == 0x10 + 27);
        memcpy(hb, "\x11\x22\x33\x44\x00\x00", 6);               /* HTC, pad */
        memcpy(hb + 6, "\xaa\xaa\x03\x00\x00\x00\x08\x06", 8);
        for (k = 0; k < 28U; ++k)
            hb[14 + k] = (uint8_t)(0x10 + k);
        len = mk_rx(p, 0x88, 0x82, OWN, AP, GW, 0, 0, hb, 42);
        p[3] = (uint8_t)(p[3] | IWM_RX_MPDU_MFLG2_PAD);
        assert(iwm_data_rx(&d, p, len, out, sizeof(out), &n) == IWM_OK);
        assert(n == 14U + 28U && out[12] == 0x08 && out[14] == 0x10);
        /* Order bit on a non-QoS frame is not an HT Control field. */
        len = mk_rx(p, 0x08, 0x82, OWN, AP, GW, 0, 0, body, 36);
        assert(iwm_data_rx(&d, p, len, out, sizeof(out), &n) == IWM_OK);
        assert(n == 14U + 28U);
    }
    iwm_data_unlink(&d);

    /* ---- DHCPDISCOVER */
    n = iwm_data_dhcp_discover(OWN, 0x12345678UL, out, sizeof(out));
    assert(n == IWM_DATA_DHCP_LEN && memcmp(out, BC, 6) == 0 &&
           memcmp(out + 6, OWN, 6) == 0 && out[12] == 0x08 && out[13] == 0);
    for (sum = 0, i = 0; i < 20; i += 2)
        sum += (uint32_t)((out[14 + i] << 8) | out[15 + i]);
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    assert(sum == 0xffff);                               /* header checksum */
    assert(out[14 + 9] == 17 && out[34] == 0 && out[35] == 68 &&
           out[36] == 0 && out[37] == 67);
    assert(out[42] == 1 && out[46] == 0x12 && out[49] == 0x78 && out[52] == 0x80);
    assert(memcmp(out + 70, OWN, 6) == 0);
    assert(memcmp(out + 278, "\x63\x82\x53\x63\x35\x01\x01", 7) == 0);
    /* Unicast-reply variant: identical except for the BOOTP flags. */
    {
        static uint8_t uni[IWM_DATA_DHCP_LEN];
        assert(iwm_data_dhcp_discover_flags(OWN, 0x12345678UL, 0, uni,
                                            sizeof(uni)) == IWM_DATA_DHCP_LEN);
        assert(uni[52] == 0 && uni[53] == 0 && out[52] == 0x80 && out[53] == 0);
        out[52] = 0;
        assert(memcmp(uni, out, IWM_DATA_DHCP_LEN) == 0);
        assert(iwm_data_dhcp_discover_flags(OWN, 0x12345678UL, IWM_DHCP_BROADCAST,
                                            uni, sizeof(uni)) == IWM_DATA_DHCP_LEN);
        assert(uni[52] == 0x80 && uni[53] == 0);
        assert(iwm_data_dhcp_discover_flags(OWN, 1, 0, uni, 341) == 0);
    }
    assert(iwm_data_dhcp_discover(OWN, 1, out, 341) == 0);

    /* Linux-identical DISCOVER: the builder reproduces the captured frame
     * byte for byte, including IP and UDP checksums. */
    {
        static uint8_t lx[IWM_DATA_DHCP_LEN];
        static const uint8_t own_lx[6] = { 0xd4, 0x3b, 0x04, 0xc3, 0x04, 0x74 };
        assert(iwm_data_dhcp_build(own_lx, 0x7965cca7UL, 0xddd9, 0, linux_opts,
                                   sizeof(linux_opts), lx, sizeof(lx)) ==
               IWM_DATA_DHCP_LEN);
        assert(memcmp(lx, linux_discover, IWM_DATA_DHCP_LEN) == 0);
        assert(lx[14 + 10] == 0x9b && lx[14 + 11] == 0xcc);     /* IP */
        assert(lx[14 + 26] == 0x24 && lx[14 + 27] == 0x3a);     /* UDP */
        assert(iwm_data_dhcp_build(own_lx, 1, 1, 0, linux_opts, 61, lx,
                                   sizeof(lx)) == 0);            /* too long */
        assert(iwm_data_dhcp_build(own_lx, 1, 1, 0, NULL, 4, lx, sizeof(lx)) == 0);
        assert(iwm_data_dhcp_build(own_lx, 1, 1, 0, linux_opts,
                                   sizeof(linux_opts), lx, 341) == 0);
        /* Default option block: same layout, own MAC as client identifier,
         * valid checksums, requested flags. */
        assert(iwm_data_dhcp_discover_full(own_lx, 0x49570005UL, 0, lx,
                                           sizeof(lx)) == IWM_DATA_DHCP_LEN);
        assert(lx[14 + 4] == 0x00 && lx[14 + 5] == 0x05);          /* IP id */
        assert(memcmp(lx + 14 + 20 + 8 + 28, own_lx, 6) == 0);
        assert(memcmp(lx + 42 + 236 + 4, "\x35\x01\x01\x37\x0b", 5) == 0);
        {
            const uint8_t *o = lx + 42 + 236 + 4;
            assert(o[3 + 13] == 57 && o[3 + 13 + 2] == 0x05 && o[3 + 13 + 3] == 0xc0);
            assert(o[3 + 13 + 4] == 61 && o[3 + 13 + 5] == 7 && o[3 + 13 + 6] == 1 &&
                   memcmp(o + 3 + 13 + 7, own_lx, 6) == 0 && o[3 + 13 + 13] == 255);
        }
        for (sum = 0, i = 0; i < 20; i += 2)
            sum += (uint32_t)((lx[14 + i] << 8) | lx[15 + i]);
        while (sum >> 16)
            sum = (sum & 0xffff) + (sum >> 16);
        assert(sum == 0xffff);                                   /* IP header */
        /* UDP: pseudo header + UDP header + data folds to all ones. */
        sum = 0;
        for (i = 0; i < 8; i += 2)
            sum += (uint32_t)((lx[14 + 12 + i] << 8) | lx[14 + 13 + i]);
        sum += 17U + 308U;
        for (i = 0; i < 308; i += 2)
            sum += (uint32_t)((lx[34 + i] << 8) | lx[35 + i]);
        while (sum >> 16)
            sum = (sum & 0xffff) + (sum >> 16);
        assert(sum == 0xffff);
        assert(iwm_data_dhcp_discover_full(own_lx, 1, IWM_DHCP_BROADCAST, lx,
                                           sizeof(lx)) == IWM_DATA_DHCP_LEN &&
               lx[52] == 0x80);
        assert(iwm_data_dhcp_discover_full(NULL, 1, 0, lx, sizeof(lx)) == 0);
    }

    /* ---- GenMAC: scan request while connected keeps cache and state */
    iwm_adapter_init(&a, NULL, NULL);
    iwm_genmac_bind_scan_cache(&a, &cache);
    a.firmware.state = IWM_FW_RUNNING;
    cache.count = 3;
    iwm_genmac_link_up(&a, AP, -30);
    {
        struct iwm_oid_request r;
        uint32_t v = 0, gen = a.status_generation;
        memset(&r, 0, sizeof(r));
        r.oid = OID_802_11_BSSID_LIST_SCAN;
        r.data = &v;
        r.data_length = 4;
        assert(iwm_genmac_oid_set(&a, &r) == IWM_OK);
        assert(cache.count == 3 && a.state == IWM_ADAPTER_ASSOCIATED &&
               a.status_generation == gen + 1);
        iwm_genmac_scan_complete(&a, IWM_OK);
        assert(a.state == IWM_ADAPTER_ASSOCIATED);
        iwm_genmac_scan_complete(&a, IWM_ETIMEDOUT);
        assert(a.state == IWM_ADAPTER_ASSOCIATED);
        iwm_genmac_link_down(&a);
        assert(iwm_genmac_oid_set(&a, &r) == IWM_OK);
        assert(cache.count == 0 && a.state == IWM_ADAPTER_SCANNING);
        iwm_genmac_scan_complete(&a, IWM_OK);
        assert(a.state == IWM_ADAPTER_READY);
    }

    puts("data path tests passed");
    return 0;
}
