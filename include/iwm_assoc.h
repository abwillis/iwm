/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef IWM_ASSOC_H
#define IWM_ASSOC_H
#include "iwm_scan.h"

/* Milestone E1: 802.11 open-system authentication and association.
 *
 * Everything here is the standard IEEE 802.11 management exchange and is
 * independent of the Intel firmware.  The firmware-facing half (MAC context,
 * PHY context, binding, station, TX data queue and TX_CMD) is milestone E2 and
 * is reached only through struct iwm_assoc_ops, so this layer is host-tested
 * against a mock.  Open networks only: a BSS that advertises privacy is
 * refused until the WPA milestone. */

/* Frame-control first byte: version 0, type 0 (management), subtype << 4. */
#define IWM_FC0_ASSOC_REQ     0x00U
#define IWM_FC0_ASSOC_RESP    0x10U
#define IWM_FC0_DISASSOC      0xa0U
#define IWM_FC0_AUTH          0xb0U
#define IWM_FC0_ACTION        0xd0U
#define IWM_80211_CAT_BLOCK_ACK 3U
#define IWM_80211_BA_ADDBA_REQ  0U
#define IWM_80211_BA_ADDBA_RESP 1U
#define IWM_FC0_DEAUTH        0xc0U
#define IWM_FC1_PROTECTED     0x40U

#define IWM_80211_STATUS_SUCCESS     0U
#define IWM_80211_AUTH_ALGO_OPEN     0U
#define IWM_80211_REASON_LEAVING     3U    /* deauthenticated: STA is leaving */

#define IWM_80211_CAP_SHORT_PREAMBLE 0x0020U
#define IWM_80211_CAP_SHORT_SLOT     0x0400U

#define IWM_80211_IE_SSID            0U
#define IWM_80211_IE_RATES           1U
#define IWM_80211_IE_EXT_RATES       50U

/* Driver policy (not 802.11 requirements). */
#define IWM_ASSOC_LISTEN_INTERVAL    10U   /* power save is off; any value */
#define IWM_ASSOC_AUTH_TIMEOUT_MS    300UL
#define IWM_ASSOC_ASSOC_TIMEOUT_MS   500UL
#define IWM_ASSOC_MAX_TRIES          3U
#define IWM_ASSOC_FRAME_MAX          160U
#define IWM_ASSOC_AID_MASK           0x3fffU

enum iwm_assoc_state {
    IWM_AS_IDLE = 0,
    IWM_AS_AUTH,      /* authentication request sent */
    IWM_AS_ASSOC,     /* association request sent */
    IWM_AS_UP,        /* associated */
    IWM_AS_FAILED
};

enum iwm_assoc_fail {
    IWM_AF_NONE = 0,
    IWM_AF_NO_BSS,          /* no matching scan result */
    IWM_AF_PRIVACY,         /* BSS requires privacy: not supported yet */
    IWM_AF_PREPARE,         /* firmware context setup failed */
    IWM_AF_NO_TX,           /* management TX path unavailable (E2) */
    IWM_AF_TX,              /* management TX failed */
    IWM_AF_AUTH_TIMEOUT,
    IWM_AF_AUTH_REFUSED,    /* status in struct iwm_assoc.status */
    IWM_AF_ASSOC_TIMEOUT,
    IWM_AF_ASSOC_REFUSED,   /* status in struct iwm_assoc.status */
    IWM_AF_DEAUTH           /* AP deauthenticated or disassociated us */
};

struct iwm_assoc_ops {
    /* Create MAC/PHY context, binding and station for the BSS (E2).  NULL:
     * nothing to prepare. */
    int (*prepare)(void *ctx, const struct iwm_scan_result *bss);
    /* Transmit one management frame; IWM_ENOTSUP when no TX path exists. */
    int (*tx_mgmt)(void *ctx, const uint8_t *frame, uint16_t length);
    /* Association completed. */
    void (*up)(void *ctx, uint16_t aid);
    /* Association ended after having been up (E2 tears down the station). */
    void (*down)(void *ctx);
};

struct iwm_assoc {
    struct iwm_adapter *adapter;
    const struct iwm_assoc_ops *ops;
    void *ctx;
    uint8_t state;
    uint8_t fail;
    uint8_t tries;
    uint8_t ssid_length;
    uint8_t ssid[32];
    /* Policy, set by the OS layer after iwm_assoc_init(): announce HT (20 MHz
     * only, no aggregation) when the BSS has an HT capabilities element. */
    uint8_t ht;
    struct iwm_scan_result bss;
    uint16_t status;          /* last status or reason code from the AP */
    uint16_t aid;
    uint16_t seq;             /* management sequence counter (12 bits used) */
    uint32_t deadline_ms;
    uint16_t frame_len;
    uint8_t frame[IWM_ASSOC_FRAME_MAX];   /* last frame built */
    uint32_t tx_auth;
    uint32_t tx_assoc;
    uint32_t rx_ignored;
};

void iwm_assoc_init(struct iwm_assoc *as, struct iwm_adapter *adapter,
                    const struct iwm_assoc_ops *ops, void *ctx);

/* Best matching infrastructure BSS in the cache: by BSSID when one is given,
 * otherwise by SSID, strongest signal first.  IWM_ENODEV when none matches. */
int iwm_assoc_select(const struct iwm_scan_cache *cache, const uint8_t *ssid,
                     uint8_t ssid_length, const uint8_t *bssid,
                     struct iwm_scan_result *out);
/* Same, restricted to BSSs whose channel is below max_khz (0: no limit). */
int iwm_assoc_select_band(const struct iwm_scan_cache *cache,
                          const uint8_t *ssid, uint8_t ssid_length,
                          const uint8_t *bssid, uint32_t max_khz,
                          struct iwm_scan_result *out);

/* An HT AP sends an ADDBA Request (Block Ack action, category 3 action 0) for
 * TID 0 right after association (Linux log: Linux answered 24 ms after its
 * association request).  Accepting needs a receive reorder buffer, which this
 * driver does not have, so the request is declined: ADDBA Response (action 1)
 * with status 37 (request declined), the dialog token, parameter set and
 * timeout echoed from the request.  Returns the length written to out (33),
 * or 0 when req is not an ADDBA Request from our BSS to this station, or the
 * state is not UP.  out is separate from as->frame because this runs from
 * the RX callback while a frame may be in flight.  An AP never sends one to a
 * station that did not announce HT. */
#define IWM_80211_STATUS_REQUEST_DECLINED 37U
uint16_t iwm_assoc_build_addba_decline(struct iwm_assoc *as,
                                       const uint8_t *req, uint16_t req_len,
                                       uint8_t *out, uint16_t cap);

/* Frame builders; return the frame length.  The frame is built into
 * as->frame (also returned through as->frame_len). */
uint16_t iwm_assoc_build_auth(struct iwm_assoc *as);
uint16_t iwm_assoc_build_assoc_req(struct iwm_assoc *as);
uint16_t iwm_assoc_build_deauth(struct iwm_assoc *as, uint16_t reason);

/* Start the exchange.  Returns IWM_OK once the authentication request has
 * been sent; otherwise the failure reason is in as->fail and the error is
 * returned.  now_ms is any monotonic millisecond clock. */
int iwm_assoc_start(struct iwm_assoc *as, const struct iwm_scan_result *bss,
                    const uint8_t *ssid, uint8_t ssid_length, uint32_t now_ms);

/* One received 802.11 frame (no RX descriptor). */
void iwm_assoc_rx(struct iwm_assoc *as, const uint8_t *frame, uint16_t length,
                  uint32_t now_ms);

/* Retransmit or give up when a deadline has passed. */
void iwm_assoc_tick(struct iwm_assoc *as, uint32_t now_ms);

/* Leave the BSS: deauthentication (best effort) and teardown. */
void iwm_assoc_stop(struct iwm_assoc *as);
#endif
