/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef IWM_DATA_H
#define IWM_DATA_H
#include "iwm_tx.h"
#include "iwm_scan.h"

/* Milestone F: data path for an associated open BSS.
 *
 * TX: an Ethernet II frame becomes an 802.11 QoS data frame (ToDS, addr1 =
 * BSSID, addr2 = own MAC, addr3 = destination, TID 0) with an RFC 1042 /
 * 802.1H SNAP header, queued on data queue 4 with the TX command Linux used
 * on this card (tx_flags 0x818, rate from the firmware rate table, retry
 * limits 60/15).  Header, flags and sequence handling match the captured
 * Linux data frames (w32.txt, mark=assoc).
 *
 * RX: a REPLY_RX_MPDU payload carrying a FromDS data frame from the BSS
 * becomes an Ethernet II frame (DA = addr1, SA = addr3).  The 2-byte pad the
 * firmware inserts after a 26-byte QoS header (descriptor mac_flags2 bit 5,
 * Linux IWL_RX_MPDU_MFLG2_PAD) is skipped.  Protected, A-MSDU, null-data and
 * own-echo frames are dropped and counted. */

#define IWM_DATA_ETH_HDR        14U
#define IWM_DATA_ETH_MAX        1514U
#define IWM_DATA_QOS_HDR        26U
#define IWM_DATA_SNAP_LEN       8U
#define IWM_DATA_FRAME_MAX      (IWM_DATA_QOS_HDR + IWM_DATA_SNAP_LEN + 1500U)
#define IWM_DATA_TID            0U
#define IWM_RX_MPDU_MFLG2_OFF   3U
#define IWM_RX_MPDU_MFLG2_PAD   0x20U

struct iwm_data {
    struct iwm_tx_transport *tx;
    uint8_t up;
    uint8_t own[6];
    uint8_t bssid[6];
    uint16_t seq;             /* 802.11 sequence number for TID 0 */
    uint32_t tx_ok;
    uint32_t tx_err;
    int32_t tx_last_rc;
    uint32_t rx_ok;
    uint32_t rx_ucast;        /* of rx_ok: addressed to this station */
    uint32_t rx_bcast;        /* of rx_ok: ff:ff:ff:ff:ff:ff */
    uint32_t rx_mcast;        /* of rx_ok: other group addresses */
    uint32_t rx_not_data;     /* management/control frames */
    uint32_t rx_other_bss;    /* not FromDS from our BSSID / not for us */
    uint32_t rx_echo;         /* our own broadcast relayed back */
    uint32_t rx_protected;
    uint32_t rx_amsdu;
    uint32_t rx_null;
    uint32_t rx_bad;          /* hardware status, truncation, non-SNAP */
};

void iwm_data_init(struct iwm_data *d, struct iwm_tx_transport *tx);
/* Associated: start converting frames for this BSS. */
void iwm_data_link(struct iwm_data *d, const uint8_t own[6],
                   const uint8_t bssid[6]);
void iwm_data_unlink(struct iwm_data *d);

/* Ethernet II -> 802.11 QoS data into out (cap bytes); returns the frame
 * length or 0 when the frame cannot be converted. */
uint16_t iwm_data_eth_to_80211(struct iwm_data *d, const uint8_t *eth,
                               uint16_t len, uint8_t *out, uint16_t cap);
/* Convert and queue on data queue 4. */
int iwm_data_tx(struct iwm_data *d, const uint8_t *eth, uint16_t len);
/* iwm_data_dhcp_discover() with a chosen BOOTP flags field: 0x0000 asks the
 * server for a reply unicast to our MAC (what Linux dhcpcd sends), 0x8000
 * (IWM_DHCP_BROADCAST, what iwm_data_dhcp_discover() sends) for a reply
 * broadcast to ff:ff:ff:ff:ff:ff.  The flags are not covered by the IP
 * header checksum and the UDP checksum is 0, so nothing else changes. */
#define IWM_DHCP_BROADCAST      0x8000U
uint16_t iwm_data_dhcp_discover_flags(const uint8_t own[6], uint32_t xid,
                                      uint16_t bootp_flags, uint8_t *out,
                                      uint16_t cap);
/* DHCPDISCOVER built the way the Linux client dhcpcd sent it on LWGuest (d26
 * capture, answered): IP header checksum AND UDP checksum, IP id and an
 * explicit option block.  opts is the option list after the magic cookie,
 * ending with 255 (at most 60 bytes); the BOOTP area is padded with zeros to
 * 300 bytes.  iwm_data_dhcp_discover() and _flags() send UDP checksum 0, which
 * RFC 768 allows but some relays and firewalls drop. */
uint16_t iwm_data_dhcp_build(const uint8_t own[6], uint32_t xid,
                             uint16_t ip_id, uint16_t bootp_flags,
                             const uint8_t *opts, uint16_t opts_len,
                             uint8_t *out, uint16_t cap);
/* The same with the default option block: 53 DISCOVER, 55 parameter request
 * list (as dhcpcd), 57 max message size 1472, 61 client identifier (type 1 +
 * own MAC).  The IP id is the low half of xid. */
uint16_t iwm_data_dhcp_discover_full(const uint8_t own[6], uint32_t xid,
                                     uint16_t bootp_flags, uint8_t *out,
                                     uint16_t cap);
/* RX MPDU payload (descriptor + frame) -> Ethernet II.
 *   IWM_OK       converted, *eth_len set
 *   IWM_ENOTSUP  not a data frame for this station (counted)
 *   IWM_EIO / IWM_EFORMAT / IWM_EOVERFLOW  dropped (counted) */
int iwm_data_rx(struct iwm_data *d, const uint8_t *payload, uint32_t len,
                uint8_t *eth, uint16_t cap, uint16_t *eth_len);

/* Data TX command template (56 bytes; length, offload, scratch set per
 * frame by iwm_tx_send_frame). */
extern const uint8_t iwm_data_txcmd[IWM_TX_TXCMD_SIZE];

/* Link test: a broadcast DHCPDISCOVER from own (Ethernet II, 342 bytes). */
#define IWM_DATA_DHCP_LEN       342U
uint16_t iwm_data_dhcp_discover(const uint8_t own[6], uint32_t xid,
                                uint8_t *out, uint16_t cap);
#endif
