/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef IWM_SCAN_H
#define IWM_SCAN_H
#include "iwm_genmac.h"

/* REPLY_RX_MPDU_CMD (0xc1) payload on the 9000 family: struct
 * iwl_rx_mpdu_desc with the v1 tail (48 bytes), then the 802.11 frame.
 * Offsets confirmed against the 9560 log: mpdu_len 0x106, mac_flags2 0x0c
 * (24-byte header), status 0x101b, energy 0x39/0x7f, channel 1, beacon frame
 * control 0x0080 at offset 48. */
#define IWM_RX_MPDU_DESC_SIZE        48U
#define IWM_RX_MPDU_LEN_OFF          0U
#define IWM_RX_MPDU_STATUS_OFF       12U
#define IWM_RX_MPDU_ENERGY_A_OFF     32U
#define IWM_RX_MPDU_ENERGY_B_OFF     33U
#define IWM_RX_MPDU_CHANNEL_OFF      34U
#define IWM_RX_MPDU_STATUS_CRC_OK    0x0001U
#define IWM_RX_MPDU_STATUS_OVERRUN_OK 0x0002U

/* 802.11 management header and beacon/probe-response fixed fields. */
#define IWM_80211_HDR_LEN            24U
#define IWM_80211_FIXED_LEN          12U   /* timestamp, interval, capability */
#define IWM_80211_SUBTYPE_PROBE_RESP 5U
#define IWM_80211_SUBTYPE_BEACON     8U
#define IWM_80211_CAP_ESS            0x0001U
#define IWM_80211_CAP_IBSS           0x0002U
#define IWM_80211_CAP_PRIVACY        0x0010U

/* Traffic indication map of a beacon (IEEE 802.11 9.4.2.6).  An AP buffers
 * frames for a station it believes is asleep and announces them here: the
 * station's AID bit in the partial virtual bitmap, and bit 0 of the bitmap
 * control byte for buffered group-addressed frames (valid on a DTIM beacon).
 * w5 log: the AP acknowledges every DHCP request and nothing comes back; a
 * set AID bit would mean the AP holds the replies because it thinks the
 * station sleeps. */
struct iwm_tim_info {
    uint8_t present;       /* a TIM element was found */
    uint8_t dtim_count;
    uint8_t dtim_period;
    uint8_t bitmap_ctl;
    uint8_t aid_set;       /* bit for the given AID is set */
    uint8_t group_set;     /* bitmap_ctl bit 0 */
};

/* frame: 802.11 beacon (frame control 0x80), length len.  IWM_OK with
 * *info filled (present 0 when there is no usable TIM), IWM_ENOTSUP when the
 * frame is not a beacon, IWM_EFORMAT when it is too short. */
int iwm_beacon_tim(const uint8_t *frame, uint32_t len, uint16_t aid,
                   struct iwm_tim_info *info);

/* Locate the 802.11 frame inside an RX MPDU payload.  IWM_OK sets *frame and
 * *frame_length; IWM_EIO when the hardware status reports a CRC error or
 * overrun, IWM_EFORMAT for a truncated descriptor or frame shorter than a
 * management header. */
int iwm_rx_mpdu_frame(const uint8_t *payload, uint32_t length,
                      const uint8_t **frame, uint32_t *frame_length);

/* Parse one RX MPDU payload into an NDIS-ready scan result.  The IE copy is
 * NDIS_802_11_FIXED_IEs (12 bytes) followed by whole IEs, cut at an IE
 * boundary to fit IWM_SCAN_MAX_IE.
 *   IWM_OK        beacon or probe response parsed into *result
 *   IWM_ENOTSUP   valid frame that is not a beacon or probe response
 *   IWM_EIO       hardware status reports a CRC error or overrun
 *   IWM_EFORMAT   truncated descriptor or frame */
int iwm_scan_parse_mpdu(const uint8_t *payload, uint32_t length,
                        struct iwm_scan_result *result);
#endif
