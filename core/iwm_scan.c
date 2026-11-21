/* SPDX-License-Identifier: GPL-2.0-only */
/* Beacon / probe-response parser for UMAC scan results (milestone D). */
#if !defined(IWM_RING0)
#include <string.h>
#endif
#include "iwm_scan.h"

/* Linux iwl_mvm_get_signal_strength(): energy is a positive magnitude, 0
 * means "no reading" (S8_MIN); the stronger chain wins. */
static int32_t chain_dbm(uint8_t energy)
{
    return energy ? -(int32_t)energy : -128;
}

static uint32_t channel_khz(uint8_t ch)
{
    if (ch == 14U)
        return 2484000UL;
    if (ch < 14U)
        return (2407UL + 5UL * ch) * 1000UL;
    return (5000UL + 5UL * ch) * 1000UL;
}

int iwm_rx_mpdu_frame(const uint8_t *p, uint32_t len,
                      const uint8_t **frame, uint32_t *frame_length)
{
    uint32_t mpdu_len;
    uint16_t status;
    if (!p || !frame || !frame_length)
        return IWM_EINVAL;
    if (len < IWM_RX_MPDU_DESC_SIZE + IWM_80211_HDR_LEN)
        return IWM_EFORMAT;
    mpdu_len = iwm_get_le16(p + IWM_RX_MPDU_LEN_OFF);
    if (mpdu_len < IWM_80211_HDR_LEN || mpdu_len > len - IWM_RX_MPDU_DESC_SIZE)
        return IWM_EFORMAT;
    status = iwm_get_le16(p + IWM_RX_MPDU_STATUS_OFF);
    if ((status & (IWM_RX_MPDU_STATUS_CRC_OK | IWM_RX_MPDU_STATUS_OVERRUN_OK)) !=
        (IWM_RX_MPDU_STATUS_CRC_OK | IWM_RX_MPDU_STATUS_OVERRUN_OK))
        return IWM_EIO;
    *frame = p + IWM_RX_MPDU_DESC_SIZE;
    *frame_length = mpdu_len;
    return IWM_OK;
}

int iwm_beacon_tim(const uint8_t *f, uint32_t len, uint16_t aid,
                   struct iwm_tim_info *t)
{
    uint32_t pos;
    if (!f || !t)
        return IWM_EINVAL;
    memset(t, 0, sizeof(*t));
    if (len < IWM_80211_HDR_LEN + IWM_80211_FIXED_LEN)
        return IWM_EFORMAT;
    if (f[0] != (uint8_t)(IWM_80211_SUBTYPE_BEACON << 4))
        return IWM_ENOTSUP;
    for (pos = IWM_80211_HDR_LEN + IWM_80211_FIXED_LEN; pos + 2U <= len;) {
        uint32_t l = f[pos + 1U];
        const uint8_t *v = f + pos + 2U;
        if (pos + 2U + l > len)
            break;
        if (f[pos] == 5U && l >= 4U) {              /* TIM */
            uint32_t n1 = (uint32_t)(v[2] & 0xfeU);  /* bitmap offset, octets */
            uint32_t octet = (uint32_t)(aid >> 3);
            t->present = 1;
            t->dtim_count = v[0];
            t->dtim_period = v[1];
            t->bitmap_ctl = v[2];
            t->group_set = (uint8_t)(v[2] & 1U);
            if (octet >= n1 && octet - n1 < l - 3U)
                t->aid_set = (uint8_t)((v[3U + octet - n1] >> (aid & 7U)) & 1U);
            return IWM_OK;
        }
        pos += 2U + l;
    }
    return IWM_OK;
}

int iwm_scan_parse_mpdu(const uint8_t *p, uint32_t len,
                        struct iwm_scan_result *r)
{
    const uint8_t *f, *body;
    uint32_t mpdu_len, blen, pos, ie_room;
    uint16_t fc, status, cap;
    uint8_t ds_chan = 0, nrates = 0;
    int32_t a, b;
    if (!p || !r)
        return IWM_EINVAL;
    if (len < IWM_RX_MPDU_DESC_SIZE + IWM_80211_HDR_LEN)
        return IWM_EFORMAT;
    mpdu_len = iwm_get_le16(p + IWM_RX_MPDU_LEN_OFF);
    if (mpdu_len > len - IWM_RX_MPDU_DESC_SIZE)
        return IWM_EFORMAT;
    status = iwm_get_le16(p + IWM_RX_MPDU_STATUS_OFF);
    if ((status & (IWM_RX_MPDU_STATUS_CRC_OK | IWM_RX_MPDU_STATUS_OVERRUN_OK)) !=
        (IWM_RX_MPDU_STATUS_CRC_OK | IWM_RX_MPDU_STATUS_OVERRUN_OK))
        return IWM_EIO;
    f = p + IWM_RX_MPDU_DESC_SIZE;
    if (mpdu_len < IWM_80211_HDR_LEN)
        return IWM_EFORMAT;
    fc = iwm_get_le16(f);
    if (((fc >> 2) & 3U) != 0 ||
        (((fc >> 4) & 0xfU) != IWM_80211_SUBTYPE_BEACON &&
         ((fc >> 4) & 0xfU) != IWM_80211_SUBTYPE_PROBE_RESP))
        return IWM_ENOTSUP;
    if (mpdu_len < IWM_80211_HDR_LEN + IWM_80211_FIXED_LEN)
        return IWM_EFORMAT;
    memset(r, 0, sizeof(*r));
    memcpy(r->bssid, f + 16, 6);              /* addr3 */
    a = chain_dbm(p[IWM_RX_MPDU_ENERGY_A_OFF]);
    b = chain_dbm(p[IWM_RX_MPDU_ENERGY_B_OFF]);
    r->rssi_dbm = a > b ? a : b;
    body = f + IWM_80211_HDR_LEN;
    blen = mpdu_len - IWM_80211_HDR_LEN;
    r->beacon_period = iwm_get_le16(body + 8);
    cap = iwm_get_le16(body + 10);
    r->privacy = (cap & IWM_80211_CAP_PRIVACY) ? 1U : 0U;
    r->infrastructure_mode = (uint8_t)((!(cap & IWM_80211_CAP_ESS) &&
                                        (cap & IWM_80211_CAP_IBSS)) ?
                                       IWM_INFRASTRUCTURE_IBSS :
                                       IWM_INFRASTRUCTURE_BSS);
    memcpy(r->ies, body, IWM_80211_FIXED_LEN);
    r->ie_length = IWM_80211_FIXED_LEN;
    r->ie_total = 0;
    ie_room = 1;
    for (pos = IWM_80211_FIXED_LEN; pos + 2U <= blen;) {
        uint8_t id = body[pos];
        uint32_t l = body[pos + 1U];
        const uint8_t *v = body + pos + 2U;
        uint32_t k;
        if (pos + 2U + l > blen)
            break;
        if (id == 0U && l <= sizeof(r->ssid)) {
            memcpy(r->ssid, v, l);
            r->ssid_length = (uint8_t)l;
        } else if (id == 1U || id == 50U) {
            for (k = 0; k < l && nrates < sizeof(r->rates); ++k)
                r->rates[nrates++] = v[k];
        } else if (id == 3U && l >= 1U) {
            ds_chan = v[0];
        } else if (id == 45U && l >= 26U) {
            r->ht = 1;                      /* HT capabilities */
        } else if (id == 221U && l >= 7U && v[0] == 0x00U && v[1] == 0x50U &&
                   v[2] == 0xf2U && v[3] == 2U && v[4] <= 1U) {
            r->wmm = 1;                     /* WMM information / parameter */
        }
        r->ie_total = (uint16_t)(r->ie_total + 2U + l);
        if (ie_room && (uint32_t)r->ie_length + 2U + l <= IWM_SCAN_MAX_IE) {
            memcpy(r->ies + r->ie_length, body + pos, 2U + l);
            r->ie_length = (uint16_t)(r->ie_length + 2U + l);
        } else {
            ie_room = 0;   /* keep whole IEs in order; stop at the first miss */
        }
        pos += 2U + l;
    }
    if (!ds_chan)
        ds_chan = p[IWM_RX_MPDU_CHANNEL_OFF];
    r->ds_config_khz = ds_chan ? channel_khz(ds_chan) : 0;
    r->network_type = (uint8_t)(ds_chan > 14U ? IWM_NETWORK_TYPE_OFDM5 :
                                                IWM_NETWORK_TYPE_OFDM24);
    return IWM_OK;
}
