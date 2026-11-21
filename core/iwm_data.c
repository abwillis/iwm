/* SPDX-License-Identifier: GPL-2.0-only */
/* Milestone F: 802.3 <-> 802.11 data path on queue 4. */
#if !defined(IWM_RING0)
#include <string.h>
#endif
#include "iwm_data.h"

/* Captured Linux data TX command (queue 4, TID 0): tx_flags 0x818,
 * rate_n_flags 0 (firmware LQ table), sta 0, life_time 0xffffffff,
 * rts retry 60, data retry 15. */
const uint8_t iwm_data_txcmd[IWM_TX_TXCMD_SIZE] = {
    0x00,0x00,0x00,0x00,0x18,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,
    0x00,0x3c,0x0f,0x00,0x00,0x00,0x00,0x00,
};

static uint8_t g_data_frame[IWM_DATA_FRAME_MAX];

static const uint8_t g_bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static int mac_eq(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6) == 0;
}

void iwm_data_init(struct iwm_data *d, struct iwm_tx_transport *tx)
{
    if (!d)
        return;
    memset(d, 0, sizeof(*d));
    d->tx = tx;
}

void iwm_data_link(struct iwm_data *d, const uint8_t own[6],
                   const uint8_t bssid[6])
{
    if (!d || !own || !bssid)
        return;
    memcpy(d->own, own, 6);
    memcpy(d->bssid, bssid, 6);
    d->seq = 0;
    d->up = 1;
}

void iwm_data_unlink(struct iwm_data *d)
{
    if (d)
        d->up = 0;
}

uint16_t iwm_data_eth_to_80211(struct iwm_data *d, const uint8_t *eth,
                               uint16_t len, uint8_t *out, uint16_t cap)
{
    uint16_t type, body, flen;
    if (!d || !eth || !out || len < IWM_DATA_ETH_HDR || len > IWM_DATA_ETH_MAX)
        return 0;
    type = (uint16_t)((eth[12] << 8) | eth[13]);
    if (type < 0x0600U)                   /* 802.3 length field: not handled */
        return 0;
    body = (uint16_t)(len - IWM_DATA_ETH_HDR);
    flen = (uint16_t)(IWM_DATA_QOS_HDR + IWM_DATA_SNAP_LEN + body);
    if (flen > cap)
        return 0;
    out[0] = 0x88;                        /* QoS data */
    out[1] = 0x01;                        /* ToDS */
    out[2] = 0;                           /* duration: set by firmware */
    out[3] = 0;
    memcpy(out + 4, d->bssid, 6);
    memcpy(out + 10, d->own, 6);
    memcpy(out + 16, eth, 6);             /* DA */
    out[22] = (uint8_t)(d->seq << 4);
    out[23] = (uint8_t)(d->seq >> 4);
    out[24] = IWM_DATA_TID;               /* QoS control: TID 0, normal ack */
    out[25] = 0;
    /* RFC 1042 SNAP; 802.1H bridge tunnel for IPX and AARP. */
    out[26] = 0xaa;
    out[27] = 0xaa;
    out[28] = 0x03;
    out[29] = 0x00;
    out[30] = 0x00;
    out[31] = (uint8_t)((type == 0x8137U || type == 0x80f3U) ? 0xf8 : 0x00);
    out[32] = eth[12];
    out[33] = eth[13];
    memcpy(out + 34, eth + IWM_DATA_ETH_HDR, body);
    return flen;
}

int iwm_data_tx(struct iwm_data *d, const uint8_t *eth, uint16_t len)
{
    uint16_t flen, seq = 0;
    int rc;
    if (!d || !d->tx)
        return IWM_EINVAL;
    if (!d->up)
        return IWM_ESTATE;
    flen = iwm_data_eth_to_80211(d, eth, len, g_data_frame,
                                 (uint16_t)sizeof(g_data_frame));
    if (!flen) {
        ++d->tx_err;
        return d->tx_last_rc = IWM_EINVAL;
    }
    rc = iwm_tx_send_frame(d->tx, IWM_TX_DATA_QUEUE, iwm_data_txcmd,
                           g_data_frame, flen, IWM_DATA_QOS_HDR, &seq);
    if (rc == IWM_OK) {
        d->seq = (uint16_t)((d->seq + 1U) & 0x0fffU);
        ++d->tx_ok;
    } else {
        ++d->tx_err;
    }
    return d->tx_last_rc = rc;
}

int iwm_data_rx(struct iwm_data *d, const uint8_t *p, uint32_t len,
                uint8_t *eth, uint16_t cap, uint16_t *eth_len)
{
    const uint8_t *f, *b;
    uint32_t flen, hdr, blen;
    uint8_t fc0, fc1;
    int rc;
    if (!d || !p || !eth || !eth_len)
        return IWM_EINVAL;
    if (!d->up)
        return IWM_ENOTSUP;
    rc = iwm_rx_mpdu_frame(p, len, &f, &flen);
    if (rc != IWM_OK) {
        ++d->rx_bad;
        return rc;
    }
    fc0 = f[0];
    fc1 = f[1];
    if ((fc0 & 0x0cU) != 0x08U) {         /* not a data frame */
        ++d->rx_not_data;
        return IWM_ENOTSUP;
    }
    /* FromDS only, sent by our AP, addressed to us or group. */
    if ((fc1 & 0x03U) != 0x02U || !mac_eq(f + 10, d->bssid) ||
        (!mac_eq(f + 4, d->own) && !(f[4] & 0x01U))) {
        ++d->rx_other_bss;
        return IWM_ENOTSUP;
    }
    if (fc1 & 0x40U) {
        ++d->rx_protected;
        return IWM_ENOTSUP;
    }
    if (fc0 & 0x40U) {                    /* null data / QoS null */
        ++d->rx_null;
        return IWM_ENOTSUP;
    }
    if (mac_eq(f + 16, d->own)) {         /* own broadcast relayed back */
        ++d->rx_echo;
        return IWM_ENOTSUP;
    }
    hdr = (fc0 & 0x80U) ? 26U : 24U;
    /* QoS data with the Order bit set carries a 4-byte HT Control field after
     * the QoS control field; an HT AP sends it for link adaptation. */
    if ((fc0 & 0x80U) && (fc1 & 0x80U))
        hdr += 4U;
    if ((fc0 & 0x80U) && (f[24] & 0x80U)) {
        ++d->rx_amsdu;
        return IWM_ENOTSUP;
    }
    if (p[IWM_RX_MPDU_MFLG2_OFF] & IWM_RX_MPDU_MFLG2_PAD)
        hdr += 2U;
    if (flen < hdr + IWM_DATA_SNAP_LEN) {
        ++d->rx_bad;
        return IWM_EFORMAT;
    }
    b = f + hdr;
    blen = flen - hdr;
    if (b[0] != 0xaa || b[1] != 0xaa || b[2] != 0x03 || b[3] != 0x00 ||
        b[4] != 0x00 || (b[5] != 0x00 && b[5] != 0xf8)) {
        ++d->rx_bad;
        return IWM_EFORMAT;
    }
    if (IWM_DATA_ETH_HDR + blen - IWM_DATA_SNAP_LEN > cap ||
        IWM_DATA_ETH_HDR + blen - IWM_DATA_SNAP_LEN > IWM_DATA_ETH_MAX) {
        ++d->rx_bad;
        return IWM_EOVERFLOW;
    }
    memcpy(eth, f + 4, 6);                /* DA */
    memcpy(eth + 6, f + 16, 6);           /* SA */
    eth[12] = b[6];
    eth[13] = b[7];
    memcpy(eth + IWM_DATA_ETH_HDR, b + IWM_DATA_SNAP_LEN,
           blen - IWM_DATA_SNAP_LEN);
    *eth_len = (uint16_t)(IWM_DATA_ETH_HDR + blen - IWM_DATA_SNAP_LEN);
    ++d->rx_ok;
    if (!(eth[0] & 0x01U))
        ++d->rx_ucast;
    else if (mac_eq(eth, g_bcast))
        ++d->rx_bcast;
    else
        ++d->rx_mcast;
    return IWM_OK;
}

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/* Ethernet 14 + IP 20 + UDP 8 + BOOTP op/htype/hlen/hops 4 + xid 4 + secs 2. */
#define IWM_DHCP_FLAGS_OFF      52U

static uint32_t csum_add(uint32_t sum, const uint8_t *p, unsigned n)
{
    unsigned i;
    for (i = 0; i + 1U < n; i += 2U)
        sum += (uint32_t)((p[i] << 8) | p[i + 1U]);
    if (n & 1U)
        sum += (uint32_t)(p[n - 1U] << 8);
    return sum;
}

static uint16_t csum_fold(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xffffUL) + (sum >> 16);
    return (uint16_t)~sum;
}

uint16_t iwm_data_dhcp_build(const uint8_t own[6], uint32_t xid,
                             uint16_t ip_id, uint16_t bootp_flags,
                             const uint8_t *opts, uint16_t opts_len,
                             uint8_t *out, uint16_t cap)
{
    uint8_t *ip, *udp, *bp;
    uint32_t sum;
    uint16_t c;
    if (!own || !out || cap < IWM_DATA_DHCP_LEN || (opts_len && !opts) ||
        opts_len > 60U)
        return 0;
    memset(out, 0, IWM_DATA_DHCP_LEN);
    memcpy(out, g_bcast, 6);
    memcpy(out + 6, own, 6);
    out[12] = 0x08;                       /* IPv4 */
    out[13] = 0x00;
    ip = out + 14;
    ip[0] = 0x45;
    put_be16(ip + 2, 328);                /* 20 + 8 + 300 */
    put_be16(ip + 4, ip_id);
    ip[8] = 64;
    ip[9] = 17;                           /* UDP */
    memset(ip + 16, 0xff, 4);             /* 0.0.0.0 -> 255.255.255.255 */
    put_be16(ip + 10, csum_fold(csum_add(0, ip, 20)));
    udp = ip + 20;
    put_be16(udp, 68);
    put_be16(udp + 2, 67);
    put_be16(udp + 4, 308);
    bp = udp + 8;
    bp[0] = 1;                            /* BOOTREQUEST */
    bp[1] = 1;                            /* Ethernet */
    bp[2] = 6;
    bp[4] = (uint8_t)(xid >> 24);
    bp[5] = (uint8_t)(xid >> 16);
    bp[6] = (uint8_t)(xid >> 8);
    bp[7] = (uint8_t)xid;
    bp[10] = (uint8_t)(bootp_flags >> 8); /* BOOTP flags: broadcast bit */
    bp[11] = (uint8_t)bootp_flags;
    memcpy(bp + 28, own, 6);              /* chaddr */
    bp[236] = 99; bp[237] = 130; bp[238] = 83; bp[239] = 99;   /* cookie */
    if (opts_len)
        memcpy(bp + 240, opts, opts_len);
    /* UDP checksum over the pseudo header (0.0.0.0, 255.255.255.255, UDP,
     * length 308), the UDP header with a zero checksum and the 300 bytes. */
    sum = csum_add(0, ip + 12, 8);
    sum += 17U + 308U;
    sum = csum_add(sum, udp, 308);
    c = csum_fold(sum);
    put_be16(udp + 6, c ? c : 0xffffU);
    return IWM_DATA_DHCP_LEN;
}

uint16_t iwm_data_dhcp_discover_full(const uint8_t own[6], uint32_t xid,
                                     uint16_t bootp_flags, uint8_t *out,
                                     uint16_t cap)
{
    uint8_t o[32];
    unsigned n = 0, i;
    static const uint8_t prl[11] = { 1, 121, 3, 6, 12, 15, 26, 28, 33, 54, 119 };
    if (!own)
        return 0;
    o[n++] = 53; o[n++] = 1; o[n++] = 1;                    /* DISCOVER */
    o[n++] = 55; o[n++] = 11;
    for (i = 0; i < sizeof(prl); ++i)
        o[n++] = prl[i];
    o[n++] = 57; o[n++] = 2; o[n++] = 0x05; o[n++] = 0xc0;  /* 1472 */
    o[n++] = 61; o[n++] = 7; o[n++] = 1;                    /* Ethernet MAC */
    for (i = 0; i < 6; ++i)
        o[n++] = own[i];
    o[n++] = 255;
    return iwm_data_dhcp_build(own, xid, (uint16_t)xid, bootp_flags, o,
                               (uint16_t)n, out, cap);
}

uint16_t iwm_data_dhcp_discover_flags(const uint8_t own[6], uint32_t xid,
                                      uint16_t bootp_flags, uint8_t *out,
                                      uint16_t cap)
{
    uint16_t n = iwm_data_dhcp_discover(own, xid, out, cap);
    if (n) {
        out[IWM_DHCP_FLAGS_OFF] = (uint8_t)(bootp_flags >> 8);
        out[IWM_DHCP_FLAGS_OFF + 1U] = (uint8_t)bootp_flags;
    }
    return n;
}

uint16_t iwm_data_dhcp_discover(const uint8_t own[6], uint32_t xid,
                                uint8_t *out, uint16_t cap)
{
    uint8_t *ip, *udp, *bp, *o;
    uint32_t sum = 0;
    unsigned i;
    if (!own || !out || cap < IWM_DATA_DHCP_LEN)
        return 0;
    memset(out, 0, IWM_DATA_DHCP_LEN);
    memcpy(out, g_bcast, 6);
    memcpy(out + 6, own, 6);
    out[12] = 0x08;                       /* IPv4 */
    out[13] = 0x00;
    ip = out + 14;
    ip[0] = 0x45;
    put_be16(ip + 2, 328);                /* 20 + 8 + 300 */
    put_be16(ip + 4, (uint16_t)xid);
    ip[8] = 64;
    ip[9] = 17;                           /* UDP */
    memset(ip + 16, 0xff, 4);             /* 0.0.0.0 -> 255.255.255.255 */
    for (i = 0; i < 20U; i += 2U)
        sum += (uint32_t)((ip[i] << 8) | ip[i + 1]);
    while (sum >> 16)
        sum = (sum & 0xffffUL) + (sum >> 16);
    put_be16(ip + 10, (uint16_t)~sum);
    udp = ip + 20;
    put_be16(udp, 68);
    put_be16(udp + 2, 67);
    put_be16(udp + 4, 308);               /* checksum 0: none (RFC 768) */
    bp = udp + 8;
    bp[0] = 1;                            /* BOOTREQUEST */
    bp[1] = 1;                            /* Ethernet */
    bp[2] = 6;
    bp[4] = (uint8_t)(xid >> 24);
    bp[5] = (uint8_t)(xid >> 16);
    bp[6] = (uint8_t)(xid >> 8);
    bp[7] = (uint8_t)xid;
    bp[10] = 0x80;                        /* broadcast reply */
    memcpy(bp + 28, own, 6);              /* chaddr */
    o = bp + 236;
    o[0] = 99; o[1] = 130; o[2] = 83; o[3] = 99;     /* magic cookie */
    o[4] = 53; o[5] = 1; o[6] = 1;                   /* DHCPDISCOVER */
    o[7] = 55; o[8] = 3; o[9] = 1; o[10] = 3; o[11] = 6;
    o[12] = 255;
    return IWM_DATA_DHCP_LEN;
}
