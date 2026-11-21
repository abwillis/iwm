/* SPDX-License-Identifier: GPL-2.0-only */
/* Host-only builder for 9000-family RX MPDU payloads (descriptor + frame). */
#ifndef IWM_TEST_BEACON_H
#define IWM_TEST_BEACON_H
#include <string.h>
#include "iwm_scan.h"

/* Writes a 48-byte iwl_rx_mpdu_desc (v1) followed by a beacon (subtype 8)
 * or probe response (5).  Returns the payload length, which like the
 * hardware carries 6 bytes past mpdu_len. */
static uint32_t tb_build(uint8_t *p, unsigned subtype, const uint8_t bssid[6],
                         const char *ssid, uint8_t desc_chan, uint8_t ds_chan,
                         uint8_t energy_a, uint16_t cap, uint16_t status)
{
    static const uint8_t rates[8] = { 0x82, 0x84, 0x8b, 0x96,
                                      0x0c, 0x12, 0x18, 0x24 };
    static const uint8_t xrates[4] = { 0x30, 0x48, 0x60, 0x6c };
    uint8_t *f = p + IWM_RX_MPDU_DESC_SIZE;
    uint8_t *b = f + IWM_80211_HDR_LEN;
    uint32_t n = IWM_80211_FIXED_LEN, sl = (uint32_t)strlen(ssid), mpdu;
    memset(p, 0, IWM_RX_MPDU_DESC_SIZE + IWM_80211_HDR_LEN + 256U);
    p[2] = 0x02;
    p[3] = 0x0c;                          /* header length 12 words */
    p[12] = (uint8_t)status;
    p[13] = (uint8_t)(status >> 8);
    p[32] = energy_a;
    p[33] = 0x7f;
    p[34] = desc_chan;
    f[0] = (uint8_t)(subtype << 4);       /* management, given subtype */
    memset(f + 4, 0xff, 6);
    memcpy(f + 10, bssid, 6);
    memcpy(f + 16, bssid, 6);
    b[8] = 100;                           /* beacon interval, TU */
    b[10] = (uint8_t)cap;
    b[11] = (uint8_t)(cap >> 8);
    b[n++] = 0; b[n++] = (uint8_t)sl; memcpy(b + n, ssid, sl); n += sl;
    b[n++] = 1; b[n++] = 8; memcpy(b + n, rates, 8); n += 8;
    if (ds_chan) { b[n++] = 3; b[n++] = 1; b[n++] = ds_chan; }
    b[n++] = 50; b[n++] = 4; memcpy(b + n, xrates, 4); n += 4;
    mpdu = IWM_80211_HDR_LEN + n;
    p[0] = (uint8_t)mpdu;
    p[1] = (uint8_t)(mpdu >> 8);
    return IWM_RX_MPDU_DESC_SIZE + mpdu + 6U;
}
#endif
