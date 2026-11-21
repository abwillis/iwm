/* SPDX-License-Identifier: GPL-2.0-only */
/* 802.11 open-system authentication and association (milestone E1). */
#if !defined(IWM_RING0)
#include <string.h>
#endif
#include "iwm_assoc.h"

/* Legacy rates in 500 kb/s units, in the order a station lists them: the
 * 2.4 GHz band carries 1, 2, 5.5, 11 then 6..54 Mb/s; 5 GHz has no CCK. */
static const uint8_t g_rates_24[12] = {
    2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108
};
static const uint8_t g_rates_5[8] = {
    12, 18, 24, 36, 48, 72, 96, 108
};

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static int mac_equal(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6) == 0;
}

/* Management header: frame control, duration 0, DA, SA, BSSID, sequence
 * control (fragment 0).  Returns a pointer to the body. */
static uint8_t *put_hdr_to(struct iwm_assoc *as, uint8_t *p, uint8_t fc0)
{
    p[0] = fc0;
    p[1] = 0;
    p[2] = 0;
    p[3] = 0;
    memcpy(p + 4, as->bss.bssid, 6);               /* DA */
    memcpy(p + 10, as->adapter->mac, 6);           /* SA */
    memcpy(p + 16, as->bss.bssid, 6);              /* BSSID */
    put_le16(p + 22, (uint16_t)((as->seq & 0x0fffU) << 4));
    as->seq = (uint16_t)((as->seq + 1U) & 0x0fffU);
    return p + 24;
}

static uint8_t *put_hdr(struct iwm_assoc *as, uint8_t fc0)
{
    return put_hdr_to(as, as->frame, fc0);
}

uint16_t iwm_assoc_build_addba_decline(struct iwm_assoc *as,
                                       const uint8_t *req, uint16_t req_len,
                                       uint8_t *out, uint16_t cap)
{
    uint8_t *b;
    if (!as || !as->adapter || !req || !out || as->state != IWM_AS_UP ||
        cap < 33U || req_len < 33U)
        return 0;
    if (req[0] != IWM_FC0_ACTION || (req[1] & IWM_FC1_PROTECTED))
        return 0;
    if (!mac_equal(req + 4, as->adapter->mac) ||
        !mac_equal(req + 10, as->bss.bssid) ||
        !mac_equal(req + 16, as->bss.bssid))
        return 0;
    if (req[24] != IWM_80211_CAT_BLOCK_ACK || req[25] != IWM_80211_BA_ADDBA_REQ)
        return 0;
    b = put_hdr_to(as, out, IWM_FC0_ACTION);
    b[0] = IWM_80211_CAT_BLOCK_ACK;
    b[1] = IWM_80211_BA_ADDBA_RESP;
    b[2] = req[26];                                  /* dialog token */
    put_le16(b + 3, IWM_80211_STATUS_REQUEST_DECLINED);
    b[5] = req[27];                                  /* BA parameter set */
    b[6] = req[28];
    b[7] = req[29];                                  /* BA timeout */
    b[8] = req[30];
    return 33;
}

uint16_t iwm_assoc_build_auth(struct iwm_assoc *as)
{
    uint8_t *b = put_hdr(as, IWM_FC0_AUTH);
    put_le16(b, IWM_80211_AUTH_ALGO_OPEN);
    put_le16(b + 2, 1);                            /* transaction 1 */
    put_le16(b + 4, IWM_80211_STATUS_SUCCESS);
    as->frame_len = 30;
    return as->frame_len;
}

/* WMM information (00:50:f2 type 2, subtype 0) or parameter (subtype 1)
 * element in the BSS's beacon/probe-response IEs.  mac80211 sets
 * assoc_data->wmm from this and then adds a WMM information element to the
 * association request; a station that did not announce WMM must not send QoS
 * data frames (fc 0x88), which iwm_data_eth_to_80211() always does. */
static int bss_has_wmm(const struct iwm_scan_result *bss)
{
    uint32_t pos = IWM_80211_FIXED_LEN;
    if (bss->wmm)
        return 1;
    while (pos + 2U <= bss->ie_length) {
        uint32_t l = bss->ies[pos + 1U];
        if (pos + 2U + l > bss->ie_length)
            break;
        if (bss->ies[pos] == 221U && l >= 7U && bss->ies[pos + 2U] == 0x00U &&
            bss->ies[pos + 3U] == 0x50U && bss->ies[pos + 4U] == 0xf2U &&
            bss->ies[pos + 5U] == 2U && bss->ies[pos + 6U] <= 1U)
            return 1;
        pos += 2U + l;
    }
    return 0;
}

/* First information element with the given id in the BSS's beacon/probe-
 * response IEs (after the 12 fixed bytes); NULL when absent or truncated. */
static const uint8_t *bss_find_ie(const struct iwm_scan_result *bss, uint8_t id,
                                  unsigned *length)
{
    uint32_t pos = IWM_80211_FIXED_LEN;
    while (pos + 2U <= bss->ie_length) {
        uint32_t l = bss->ies[pos + 1U];
        if (pos + 2U + l > bss->ie_length)
            break;
        if (bss->ies[pos] == id) {
            if (length)
                *length = (unsigned)l;
            return bss->ies + pos + 2U;
        }
        pos += 2U + l;
    }
    return NULL;
}

/* Rates this card transmits, in 500 kb/s units.  CCK (1, 2, 5.5, 11 Mb/s)
 * exists on 2.4 GHz only. */
static int rate_usable(uint8_t code, int five_ghz)
{
    switch (code) {
    case 2: case 4: case 11: case 22:
        return !five_ghz;
    case 12: case 18: case 24: case 36: case 48: case 72: case 96: case 108:
        return 1;
    default:
        return 0;
    }
}

/* The rates the BSS advertises (supported + extended supported, in the
 * BSS's order), with the basic-rate bit removed and anything this card cannot
 * transmit (BSS membership selectors such as 0xff for HT) dropped.  This is
 * what Linux mac80211 and BSD net80211 send: the intersection with the BSS's
 * rate set, not a fixed table.  d27 log: the AP of LWGuest advertises
 * 9 12* 18 24 36 48 54 Mb/s (no CCK, no 6 Mb/s), the driver announced
 * 1 2 5.5 11 6 9 12 18 24 36 48 54, and Linux announced exactly the AP's
 * seven rates (12 18 24 30 48 60 6c). */
static unsigned bss_rates(const struct iwm_scan_result *bss, uint8_t *out)
{
    unsigned i, k, n = 0;
    int five = bss->network_type == IWM_NETWORK_TYPE_OFDM5;
    for (i = 0; i < sizeof(bss->rates); ++i) {
        uint8_t code = (uint8_t)(bss->rates[i] & 0x7fU);
        if (!rate_usable(code, five))
            continue;
        for (k = 0; k < n && out[k] != code; ++k)
            ;
        if (k == n)
            out[n++] = code;
    }
    return n;
}

/* HT capabilities element announced when as->ht is set and the BSS has one:
 * the element Linux sent on this card (d26 capture) with every 40 MHz item
 * removed, because the PHY context replayed by iwm_sta.c is 20 MHz wide:
 * channel width set 20 MHz only, no SGI-40, no 40 MHz DSSS/CCK, maximum
 * A-MSDU length 3839.  Kept from Linux: LDPC, dynamic SM power save, SGI-20,
 * TX STBC, one-stream RX STBC, A-MPDU factor 3 / density 5, MCS 0-15. */
static const uint8_t g_ht20_ie[28] = {
    45, 26,
    0xa5, 0x01,                                      /* HT capability info */
    0x17,                                            /* A-MPDU parameters */
    0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* RX MCS */
    0x90, 0x00,                                      /* highest rate 144 */
    0x01, 0x00, 0x00, 0x00,                          /* TX MCS defined */
    0x00, 0x00,                                      /* HT extended caps */
    0x00, 0x00, 0x00, 0x00,                          /* TxBF */
    0x00                                             /* ASEL */
};

uint16_t iwm_assoc_build_assoc_req(struct iwm_assoc *as)
{
    static uint8_t bssr[16];   /* static: ring-0 stack is small */
    const uint8_t *rates;
    unsigned nrates, i, first, rest, hl = 0;
    uint16_t cap = 0x0001U;                        /* ESS */
    uint8_t *b = put_hdr(as, IWM_FC0_ASSOC_REQ);
    uint8_t *p;
    if (as->bss.ie_length >= IWM_80211_FIXED_LEN)
        cap |= (uint16_t)(iwm_get_le16(as->bss.ies + 10) &
                          (IWM_80211_CAP_SHORT_PREAMBLE |
                           IWM_80211_CAP_SHORT_SLOT));
    put_le16(b, cap);
    put_le16(b + 2, IWM_ASSOC_LISTEN_INTERVAL);
    p = b + 4;
    *p++ = IWM_80211_IE_SSID;
    *p++ = as->ssid_length;
    memcpy(p, as->ssid, as->ssid_length);
    p += as->ssid_length;
    nrates = bss_rates(&as->bss, bssr);
    if (nrates) {
        rates = bssr;
    } else if (as->bss.network_type == IWM_NETWORK_TYPE_OFDM5) {
        rates = g_rates_5;                  /* no rate IEs in the cache entry */
        nrates = sizeof(g_rates_5);
    } else {
        rates = g_rates_24;
        nrates = sizeof(g_rates_24);
    }
    first = nrates > 8U ? 8U : nrates;
    rest = nrates - first;
    *p++ = IWM_80211_IE_RATES;
    *p++ = (uint8_t)first;
    for (i = 0; i < first; ++i)
        *p++ = rates[i];
    if (rest) {
        *p++ = IWM_80211_IE_EXT_RATES;
        *p++ = (uint8_t)rest;
        for (i = 0; i < rest; ++i)
            *p++ = rates[first + i];
    }
    if (as->ht && (as->bss.ht ||
                   (bss_find_ie(&as->bss, 45, &hl) != NULL && hl >= 26U))) {
        memcpy(p, g_ht20_ie, sizeof(g_ht20_ie));
        p += sizeof(g_ht20_ie);
    }
    if (bss_has_wmm(&as->bss)) {
        *p++ = 221;                                  /* vendor specific */
        *p++ = 7;
        *p++ = 0x00; *p++ = 0x50; *p++ = 0xf2;       /* Microsoft OUI */
        *p++ = 2;                                    /* WMM */
        *p++ = 0;                                    /* information element */
        *p++ = 1;                                    /* version */
        *p++ = 0;                                    /* QoS info: no U-APSD */
    }
    as->frame_len = (uint16_t)(p - as->frame);
    return as->frame_len;
}

uint16_t iwm_assoc_build_deauth(struct iwm_assoc *as, uint16_t reason)
{
    uint8_t *b = put_hdr(as, IWM_FC0_DEAUTH);
    put_le16(b, reason);
    as->frame_len = 26;
    return as->frame_len;
}

void iwm_assoc_init(struct iwm_assoc *as, struct iwm_adapter *adapter,
                    const struct iwm_assoc_ops *ops, void *ctx)
{
    if (!as)
        return;
    memset(as, 0, sizeof(*as));
    as->adapter = adapter;
    as->ops = ops;
    as->ctx = ctx;
    as->state = IWM_AS_IDLE;
}

int iwm_assoc_select(const struct iwm_scan_cache *cache, const uint8_t *ssid,
                     uint8_t ssid_length, const uint8_t *bssid,
                     struct iwm_scan_result *out)
{
    return iwm_assoc_select_band(cache, ssid, ssid_length, bssid, 0, out);
}

int iwm_assoc_select_band(const struct iwm_scan_cache *cache,
                          const uint8_t *ssid, uint8_t ssid_length,
                          const uint8_t *bssid, uint32_t max_khz,
                          struct iwm_scan_result *out)
{
    int best = -1;
    uint16_t i;
    if (!cache || !out || (!bssid && (!ssid || ssid_length == 0)))
        return IWM_EINVAL;
    for (i = 0; i < cache->count; ++i) {
        const struct iwm_scan_result *r = &cache->result[i];
        if (r->infrastructure_mode != IWM_INFRASTRUCTURE_BSS)
            continue;
        if (max_khz && r->ds_config_khz >= max_khz)
            continue;
        if (bssid) {
            if (!mac_equal(r->bssid, bssid))
                continue;
        } else if (r->ssid_length != ssid_length ||
                   memcmp(r->ssid, ssid, ssid_length) != 0) {
            continue;
        }
        if (best < 0 || r->rssi_dbm > cache->result[best].rssi_dbm)
            best = (int)i;
    }
    if (best < 0)
        return IWM_ENODEV;
    memcpy(out, &cache->result[best], sizeof(*out));
    return IWM_OK;
}

static int fail(struct iwm_assoc *as, uint8_t reason, int rc)
{
    as->state = IWM_AS_FAILED;
    as->fail = reason;
    return rc;
}

static int send_frame(struct iwm_assoc *as)
{
    if (!as->ops || !as->ops->tx_mgmt)
        return IWM_ENOTSUP;
    return as->ops->tx_mgmt(as->ctx, as->frame, as->frame_len);
}

static int send_auth(struct iwm_assoc *as, uint32_t now_ms)
{
    int rc;
    iwm_assoc_build_auth(as);
    rc = send_frame(as);
    if (rc != IWM_OK)
        return fail(as, rc == IWM_ENOTSUP ? IWM_AF_NO_TX : IWM_AF_TX, rc);
    ++as->tries;
    ++as->tx_auth;
    as->deadline_ms = now_ms + IWM_ASSOC_AUTH_TIMEOUT_MS;
    return IWM_OK;
}

static int send_assoc(struct iwm_assoc *as, uint32_t now_ms)
{
    int rc;
    iwm_assoc_build_assoc_req(as);
    rc = send_frame(as);
    if (rc != IWM_OK)
        return fail(as, rc == IWM_ENOTSUP ? IWM_AF_NO_TX : IWM_AF_TX, rc);
    ++as->tries;
    ++as->tx_assoc;
    as->deadline_ms = now_ms + IWM_ASSOC_ASSOC_TIMEOUT_MS;
    return IWM_OK;
}

int iwm_assoc_start(struct iwm_assoc *as, const struct iwm_scan_result *bss,
                    const uint8_t *ssid, uint8_t ssid_length, uint32_t now_ms)
{
    int rc;
    if (!as || !as->adapter || !bss || !ssid || ssid_length == 0 ||
        ssid_length > sizeof(as->ssid))
        return IWM_EINVAL;
    if (!as->adapter->mac_valid)
        return IWM_ESTATE;
    memcpy(&as->bss, bss, sizeof(as->bss));
    memcpy(as->ssid, ssid, ssid_length);
    as->ssid_length = ssid_length;
    as->fail = IWM_AF_NONE;
    as->tries = 0;
    as->status = 0;
    as->aid = 0;
    if (bss->privacy)
        return fail(as, IWM_AF_PRIVACY, IWM_ENOTSUP);
    as->state = IWM_AS_AUTH;
    if (as->ops && as->ops->prepare) {
        rc = as->ops->prepare(as->ctx, &as->bss);
        if (rc != IWM_OK)
            return fail(as, IWM_AF_PREPARE, rc);
    }
    return send_auth(as, now_ms);
}

/* Addressed to this station (or broadcast) and sent by the BSS. */
static int frame_ok(const struct iwm_assoc *as, const uint8_t *f)
{
    static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    return (mac_equal(f + 4, as->adapter->mac) || mac_equal(f + 4, bcast)) &&
           mac_equal(f + 10, as->bss.bssid) &&
           mac_equal(f + 16, as->bss.bssid);
}

void iwm_assoc_rx(struct iwm_assoc *as, const uint8_t *f, uint16_t len,
                  uint32_t now_ms)
{
    const uint8_t *b;
    if (!as || !f || len < 24U || as->state == IWM_AS_IDLE ||
        as->state == IWM_AS_FAILED) {
        if (as)
            ++as->rx_ignored;
        return;
    }
    if ((f[1] & IWM_FC1_PROTECTED) || !frame_ok(as, f)) {
        ++as->rx_ignored;
        return;
    }
    b = f + 24;
    if (f[0] == IWM_FC0_DEAUTH || f[0] == IWM_FC0_DISASSOC) {
        if (len < 26U) {
            ++as->rx_ignored;
            return;
        }
        as->status = iwm_get_le16(b);
        if (as->state == IWM_AS_UP && as->ops && as->ops->down)
            as->ops->down(as->ctx);
        (void)fail(as, IWM_AF_DEAUTH, IWM_OK);
        return;
    }
    if (f[0] == IWM_FC0_AUTH && as->state == IWM_AS_AUTH) {
        if (len < 30U || iwm_get_le16(b) != IWM_80211_AUTH_ALGO_OPEN ||
            iwm_get_le16(b + 2) != 2U) {
            ++as->rx_ignored;
            return;
        }
        as->status = iwm_get_le16(b + 4);
        if (as->status != IWM_80211_STATUS_SUCCESS) {
            (void)fail(as, IWM_AF_AUTH_REFUSED, IWM_OK);
            return;
        }
        as->state = IWM_AS_ASSOC;
        as->tries = 0;
        (void)send_assoc(as, now_ms);
        return;
    }
    if (f[0] == IWM_FC0_ASSOC_RESP && as->state == IWM_AS_ASSOC) {
        if (len < 30U) {
            ++as->rx_ignored;
            return;
        }
        as->status = iwm_get_le16(b + 2);
        if (as->status != IWM_80211_STATUS_SUCCESS) {
            (void)fail(as, IWM_AF_ASSOC_REFUSED, IWM_OK);
            return;
        }
        as->aid = (uint16_t)(iwm_get_le16(b + 4) & IWM_ASSOC_AID_MASK);
        as->state = IWM_AS_UP;
        if (as->ops && as->ops->up)
            as->ops->up(as->ctx, as->aid);
        return;
    }
    ++as->rx_ignored;
}

void iwm_assoc_tick(struct iwm_assoc *as, uint32_t now_ms)
{
    if (!as || (as->state != IWM_AS_AUTH && as->state != IWM_AS_ASSOC))
        return;
    if ((int32_t)(now_ms - as->deadline_ms) < 0)
        return;
    if (as->tries >= IWM_ASSOC_MAX_TRIES) {
        (void)fail(as, as->state == IWM_AS_AUTH ? IWM_AF_AUTH_TIMEOUT :
                                                   IWM_AF_ASSOC_TIMEOUT,
                   IWM_ETIMEDOUT);
        return;
    }
    if (as->state == IWM_AS_AUTH)
        (void)send_auth(as, now_ms);
    else
        (void)send_assoc(as, now_ms);
}

void iwm_assoc_stop(struct iwm_assoc *as)
{
    int was_up;
    if (!as || as->state == IWM_AS_IDLE)
        return;
    was_up = as->state == IWM_AS_UP;
    if (was_up || as->state == IWM_AS_ASSOC) {
        iwm_assoc_build_deauth(as, IWM_80211_REASON_LEAVING);
        (void)send_frame(as);
    }
    if (was_up && as->ops && as->ops->down)
        as->ops->down(as->ctx);
    as->state = IWM_AS_IDLE;
    as->fail = IWM_AF_NONE;
    as->aid = 0;
}
