/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef IWM_STA_H
#define IWM_STA_H
#include "iwm_rt_fw.h"

/* Milestone E2: firmware side of station association (PHY context, binding,
 * MAC context, AP station, time event, management queue 5, TX command), the
 * part reached through struct iwm_assoc_ops.  Every command is the Linux
 * iwlwifi command captured on this card (core/iwm_sta_seq.c) with the own
 * MAC, BSSID, channel, beacon interval, AID and time-event id patched.  The
 * capture is 2.4 GHz only; a 5 GHz BSS is refused (IWM_ENOTSUP). */

#define IWM_SS_NONE         0xffffU
#define IWM_SS_NONFATAL     0x0001U  /* log the result and continue */
#define IWM_SS_PHY_CHAN     0x0002U  /* le32 channel at 16, band at 20 */
#define IWM_SS_MACCTX       0x0004U  /* bi 0x74, dtim interval 0x7c, aid 0x88 */
#define IWM_SS_TE_ADD       0x0008U  /* response le32 at 8 = time-event id */
#define IWM_SS_TE_REMOVE    0x0010U  /* le32 at 8 = time-event id */
#define IWM_SS_SET_PTR      0x0020U  /* SCD_QUEUE_CFG enable: queue ptr 0 first */
#define IWM_SS_SKIP         0x0040U  /* captured, but not sent (see iwm_sta.c) */

#define IWM_STA_PRE         12U
#define IWM_STA_AUTHED      2U
#define IWM_STA_UP          19U
#define IWM_STA_TE_DONE     1U
#define IWM_STA_DOWN        26U
#define IWM_STA_ABORT       7U

#define IWM_STA_PHY_CHAN_OFF   16U
#define IWM_STA_PHY_BAND_OFF   20U
#define IWM_STA_PHY_BAND_24    1U   /* capture value for 2412 MHz */
#define IWM_STA_MAC_FILTER_OFF 52U   /* MAC context filter_flags */
#define IWM_MAC_FILTER_IN_PROMISC          0x1UL
#define IWM_MAC_FILTER_IN_CONTROL_AND_MGMT 0x2UL
#define IWM_STA_MAC_BI_OFF     0x74U
#define IWM_STA_MAC_DTIM_OFF   0x7cU
#define IWM_STA_MAC_AID_OFF    0x88U
#define IWM_STA_TE_ID_OFF      8U
#define IWM_STA_SCD_QUEUE_OFF  3U
#define IWM_STA_TX_PM_TIMEOUT_OFF 52U
#define IWM_STA_CMD_TIME_EVENT_NOTIF 0x2aU
#define IWM_STA_TE_WAIT_US     1000000UL
#define IWM_STA_LOG            96U   /* d20: 48 hid 12 of 26 teardown commands */
#define IWM_STA_TXRESP_LOG     4U
#define IWM_STA_TXRESP_BYTES   48U

struct iwm_sta_seq_cmd {
    uint16_t id;
    uint16_t len;
    const uint8_t *data;      /* NULL: all zero; trailing zeros implied */
    uint16_t mac_off;         /* own MAC, or IWM_SS_NONE */
    uint16_t bssid_off;       /* BSSID, or IWM_SS_NONE */
    uint16_t flags;
};

extern const struct iwm_sta_seq_cmd iwm_sta_seq_pre[IWM_STA_PRE];
extern const struct iwm_sta_seq_cmd iwm_sta_seq_auth[IWM_STA_AUTHED];
extern const struct iwm_sta_seq_cmd iwm_sta_seq_up[IWM_STA_UP];
extern const struct iwm_sta_seq_cmd iwm_sta_seq_te[IWM_STA_TE_DONE];
extern const struct iwm_sta_seq_cmd iwm_sta_seq_down[IWM_STA_DOWN];
extern const struct iwm_sta_seq_cmd iwm_sta_seq_fail[IWM_STA_ABORT];
extern const uint8_t iwm_sta_mgmt_txcmd[IWM_TX_TXCMD_SIZE];

enum iwm_sta_phase {
    IWM_STP_IDLE = 0,
    IWM_STP_PREPARED,     /* contexts, station and queue 5 in place */
    IWM_STP_AUTHED,       /* post-authentication commands sent */
    IWM_STP_UP,           /* associated configuration sent */
    IWM_STP_FAILED
};

struct iwm_sta_log {
    uint16_t id;
    uint16_t resp_len;
    int32_t rc;
    uint32_t resp0;
    uint32_t resp1;
    uint32_t resp2;
};

struct iwm_sta {
    struct iwm_rt_fw *rt;
    struct iwm_tx_transport *tx;
    uint8_t phase;
    uint8_t mac[6];
    uint8_t bssid[6];
    uint8_t channel;
    uint8_t te_active;        /* time event added and not yet removed */
    uint8_t te_started;       /* TIME_EVENT notification seen */
    uint16_t aid;
    uint16_t beacon_int;
    /* Bits ORed into filter_flags (offset IWM_STA_MAC_FILTER_OFF) of every MAC
     * context the replay builds, from the first one on (build flag
     * IWM_RXPROMISC: promiscuous 0x1, control and management 0x2).  w6 log: a
     * MAC context sent once more after association, to add these bits, cost
     * the firmware the beacons of the BSS (31-32 MISSED_BEACONS notifications)
     * and every data TX response, so the experiment proved nothing; here the
     * replayed sequence is unchanged apart from the filter word.  Set before
     * iwm_sta_prepare(); survives prepare and teardown. */
    uint32_t filter_or;
    uint32_t te_id;
    int32_t rc;
    uint16_t fail_id;
    uint32_t te_wait_us;
    uint8_t te_notif[24];
    uint16_t te_notif_len;
    /* every command sent, in order */
    struct iwm_sta_log log[IWM_STA_LOG];
    uint16_t log_n;
    /* management TX */
    uint32_t tx_sent;
    uint32_t tx_resp;
    uint16_t txresp_len[IWM_STA_TXRESP_LOG];
    uint16_t txresp_seq[IWM_STA_TXRESP_LOG];
    uint8_t txresp[IWM_STA_TXRESP_LOG][IWM_STA_TXRESP_BYTES];
    uint8_t txresp_n;
};

void iwm_sta_init(struct iwm_sta *sta, struct iwm_rt_fw *rt,
                  struct iwm_tx_transport *tx);
/* Channel number for a 2.4 GHz frequency in kHz, 0 otherwise. */
uint8_t iwm_sta_channel_24(uint32_t khz);
/* Notification hook (install with iwm_rt_fw_set_notif_hook): time-event
 * notification and TX responses of queues 4/5. */
void iwm_sta_notif(void *ctx, uint8_t cmd, uint8_t group, uint16_t seq,
                   const uint8_t *payload, uint32_t length);
/* assoc prepare(): commands 61-72, then wait up to 1 s for the time event to
 * start (logged, not fatal). */
int iwm_sta_prepare(struct iwm_sta *sta, const struct iwm_scan_result *bss,
                    const uint8_t mac[6]);
/* assoc tx_mgmt(): a management frame on queue 5.  Before the first
 * association request the post-authentication commands 73-74 are sent. */
int iwm_sta_tx_mgmt(struct iwm_sta *sta, const uint8_t *frame, uint16_t length);
/* assoc up(): commands 75-94 with the AID, then the time-event removal. */
int iwm_sta_up(struct iwm_sta *sta, uint16_t aid);
/* Undo: full disconnect sequence after UP, the short one otherwise. */
void iwm_sta_teardown(struct iwm_sta *sta);
/* Build one table entry into out (exposed for host tests). */
int iwm_sta_build(const struct iwm_sta *sta, const struct iwm_sta_seq_cmd *c,
                  uint8_t *out);
#endif
