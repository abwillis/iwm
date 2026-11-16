/* SPDX-License-Identifier: GPL-2.0-only */
/* Ring 0: Dev32lib.h (forced in with -fi) declares memcpy/memset and the
 * miniddk string.h conflicts with it. */
#if !defined(IWM_RING0)
#include <string.h>
#endif
#include "iwm_genmac.h"

static int put(struct iwm_oid_request *r, const void *src, uint32_t length)
{
    if (!r->data || r->data_length < length)
        return IWM_EOVERFLOW;
    memcpy(r->data, src, length);
    r->bytes_done = length;
    r->ndis_status = 0;
    return IWM_OK;
}

static int put_u32(struct iwm_oid_request *r, uint32_t value)
{
    return put(r, &value, sizeof(value));
}

static int get_u32(const struct iwm_oid_request *r, uint32_t *value)
{
    if (!r->data || r->data_length < sizeof(*value))
        return IWM_EINVAL;
    memcpy(value, r->data, sizeof(*value));
    return IWM_OK;
}

static int mac_is_zero(const uint8_t *mac)
{
    unsigned i;
    uint8_t v = 0;
    for (i = 0; i < 6; ++i)
        v |= mac[i];
    return v == 0;
}

/* NDIS_WLAN_BSSID_EX has a fixed 116-byte prefix before its variable IEs.
 * All ULONG fields are naturally four-byte aligned by the explicit two-byte
 * Reserved field following the BSSID. */
#define IWM_BSSID_EX_FIXED 116U

static void store_u32(uint8_t *p, uint32_t value)
{
    memcpy(p, &value, sizeof(value));
}

static uint32_t scan_list_size(const struct iwm_scan_cache *cache)
{
    uint32_t size = 4;
    uint16_t i;
    if (!cache)
        return size;
    for (i = 0; i < cache->count; ++i)
        size += IWM_ALIGN_UP(IWM_BSSID_EX_FIXED + cache->result[i].ie_length, 4U);
    return size;
}

static int put_scan_list(struct iwm_adapter *a, struct iwm_oid_request *r)
{
    const struct iwm_scan_cache *cache = a->scan_cache;
    uint32_t required = scan_list_size(cache);
    uint8_t *out;
    uint32_t off;
    uint16_t i;

    if (!r->data || r->data_length < required) {
        r->bytes_done = required;
        return IWM_EOVERFLOW;
    }
    out = (uint8_t *)r->data;
    memset(out, 0, required);
    store_u32(out, cache ? cache->count : 0);
    off = 4;
    if (cache) {
        for (i = 0; i < cache->count; ++i) {
            const struct iwm_scan_result *s = &cache->result[i];
            uint32_t record = IWM_ALIGN_UP(IWM_BSSID_EX_FIXED + s->ie_length, 4U);
            store_u32(out + off, record);
            memcpy(out + off + 4, s->bssid, sizeof(s->bssid));
            store_u32(out + off + 12, s->ssid_length);
            memcpy(out + off + 16, s->ssid, s->ssid_length);
            store_u32(out + off + 48, s->privacy ? 1UL : 0UL);
            memcpy(out + off + 52, &s->rssi_dbm, sizeof(s->rssi_dbm));
            store_u32(out + off + 56, s->network_type);
            store_u32(out + off + 60, 32); /* NDIS_802_11_CONFIGURATION */
            store_u32(out + off + 64, s->beacon_period);
            store_u32(out + off + 72, s->ds_config_khz);
            store_u32(out + off + 76, 16); /* FH configuration length */
            store_u32(out + off + 92, s->infrastructure_mode);
            memcpy(out + off + 96, s->rates, sizeof(s->rates));
            store_u32(out + off + 112, s->ie_length);
            if (s->ie_length)
                memcpy(out + off + IWM_BSSID_EX_FIXED, s->ies, s->ie_length);
            off += record;
        }
    }
    r->bytes_done = required;
    r->ndis_status = 0;
    return IWM_OK;
}

int iwm_genmac_oid_get(struct iwm_adapter *a, struct iwm_oid_request *r)
{
    struct iwm_ndis_ssid ssid;
    if (!a || !r)
        return IWM_EINVAL;
    switch (r->oid) {
    case OID_GEN_HARDWARE_STATUS:
        return put_u32(r, IWM_NDIS_HARDWARE_READY);
    case OID_802_3_PERMANENT_ADDRESS:
    case OID_802_3_CURRENT_ADDRESS:
        if (!a->mac_valid || mac_is_zero(a->mac)) {
            r->ndis_status = IWM_NDIS_STATUS_ADAPTER_NOT_READY;
            return IWM_ESTATE;
        }
        return put(r, a->mac, sizeof(a->mac));
    case OID_PRIVATE_WRAPPER_LANNUMBER: return put_u32(r, a->lan_number);
    case OID_PRIVATE_WRAPPER_ISWIRELESS: return put_u32(r, 1);
    case OID_PRIVATE_WRAPPER_LINKSTATUS: return put_u32(r, a->connected ? 1 : 0);
    case OID_PRIVATE_WRAPPER_STATUSCHANGE: return put_u32(r, a->status_generation);
    case OID_PRIVATE_WRAPPER_GENMAC_VERSION: return put_u32(r, IWM_GENMAC_VERSION);
    case OID_PRIVATE_WRAPPER_WINDRIVER_NIFNAME:
        return put(r, "IWMAC.NIF", 10);
    case OID_GEN_LINK_SPEED:
        /* NDIS reports units of 100 bits/second. */
        return put_u32(r, a->link_speed_kbps * 10UL);
    case OID_GEN_MEDIA_CONNECT_STATUS:
        return put_u32(r, a->connected ? IWM_MEDIA_CONNECTED :
                                      IWM_MEDIA_DISCONNECTED);
    case OID_802_11_RSSI:
        return put(r, &a->rssi_dbm, sizeof(a->rssi_dbm));
    case OID_802_11_BSSID:
        return put(r, a->bssid, sizeof(a->bssid));
    case OID_802_11_SSID:
        memset(&ssid, 0, sizeof(ssid));
        ssid.length = a->ssid_length;
        memcpy(ssid.ssid, a->ssid, a->ssid_length);
        return put(r, &ssid, sizeof(ssid));
    case OID_802_11_POWER_MODE:
        return put_u32(r, a->power_mode);
    case OID_802_11_INFRASTRUCTURE_MODE:
        return put_u32(r, a->infrastructure_mode);
    case OID_802_11_NETWORK_TYPE_IN_USE:
        return put_u32(r, a->network_type);
    case OID_802_11_AUTHENTICATION_MODE:
        return put_u32(r, a->authentication_mode);
    case OID_802_11_ENCRYPTION_STATUS:
        return put_u32(r, a->encryption_status);
    case OID_802_11_PRIVACY_FILTER:
        return put_u32(r, a->privacy_filter);
    case OID_802_11_BSSID_LIST:
        return put_scan_list(a, r);
    default:
        r->ndis_status = IWM_NDIS_STATUS_NOT_SUPPORTED;
        return IWM_ENOTSUP;
    }
}

int iwm_genmac_oid_set(struct iwm_adapter *a, struct iwm_oid_request *r)
{
    const struct iwm_ndis_ssid *ssid;
    uint32_t value;
    if (!a || !r)
        return IWM_EINVAL;
    switch (r->oid) {
    case OID_802_11_SSID:
        if (!r->data || r->data_length < sizeof(*ssid)) return IWM_EINVAL;
        ssid = (const struct iwm_ndis_ssid *)r->data;
        if (ssid->length > sizeof(a->ssid)) return IWM_EINVAL;
        memcpy(a->ssid, ssid->ssid, ssid->length);
        a->ssid_length = (uint8_t)ssid->length;
        ++a->status_generation;
        r->bytes_done = sizeof(*ssid);
        return IWM_OK;
    case OID_802_11_BSSID_LIST_SCAN:
        if (a->firmware.state != IWM_FW_RUNNING) return IWM_ESTATE;
        iwm_genmac_scan_reset(a);
        a->state = IWM_ADAPTER_SCANNING;
        ++a->status_generation;
        return IWM_OK;
    case OID_802_11_DISASSOCIATE:
        a->connected = 0;
        a->state = IWM_ADAPTER_READY;
        memset(a->bssid, 0, sizeof(a->bssid));
        ++a->status_generation;
        return IWM_OK;
    case OID_802_11_INFRASTRUCTURE_MODE:
        if (!r->data || r->data_length < sizeof(value)) return IWM_EINVAL;
        memcpy(&value, r->data, sizeof(value));
        if (value > IWM_INFRASTRUCTURE_AUTO) return IWM_EINVAL;
        if (a->infrastructure_mode != value) {
            a->infrastructure_mode = value;
            ++a->status_generation;
        }
        r->bytes_done = sizeof(value);
        return IWM_OK;
    case OID_802_11_POWER_MODE:
        if (get_u32(r, &value) != IWM_OK || value > 2UL)
            return IWM_EINVAL;
        a->power_mode = value;
        r->bytes_done = sizeof(value);
        return IWM_OK;
    case OID_802_11_AUTHENTICATION_MODE:
        if (get_u32(r, &value) != IWM_OK || value >= IWM_AUTH_MAX)
            return IWM_EINVAL;
        a->authentication_mode = value;
        r->bytes_done = sizeof(value);
        return IWM_OK;
    case OID_802_11_ENCRYPTION_STATUS:
        if (get_u32(r, &value) != IWM_OK ||
            value > IWM_ENCRYPTION_AES_KEY_ABSENT)
            return IWM_EINVAL;
        a->encryption_status = value;
        r->bytes_done = sizeof(value);
        return IWM_OK;
    case OID_802_11_PRIVACY_FILTER:
        if (get_u32(r, &value) != IWM_OK || value > IWM_PRIVACY_8021X_WEP)
            return IWM_EINVAL;
        a->privacy_filter = value;
        r->bytes_done = sizeof(value);
        return IWM_OK;
    case OID_802_11_ADD_KEY:
    case OID_802_11_REMOVE_KEY:
        /* Key-command construction is a required milestone before WPA traffic. */
        r->ndis_status = IWM_NDIS_STATUS_NOT_SUPPORTED;
        return IWM_ENOTSUP;
    default:
        r->ndis_status = IWM_NDIS_STATUS_NOT_SUPPORTED;
        return IWM_ENOTSUP;
    }
}

void iwm_genmac_bind_scan_cache(struct iwm_adapter *a,
                                struct iwm_scan_cache *cache)
{
    if (!a)
        return;
    a->scan_cache = cache;
    if (cache)
        memset(cache, 0, sizeof(*cache));
}

void iwm_genmac_scan_reset(struct iwm_adapter *a)
{
    if (!a || !a->scan_cache)
        return;
    a->scan_cache->count = 0;
    ++a->scan_cache->generation;
}

int iwm_genmac_scan_add(struct iwm_adapter *a,
                        const struct iwm_scan_result *result)
{
    struct iwm_scan_cache *cache;
    struct iwm_scan_result *dst;
    if (!a || !result || !a->scan_cache)
        return IWM_EINVAL;
    if (result->ssid_length > sizeof(result->ssid) ||
        result->ie_length > sizeof(result->ies))
        return IWM_EINVAL;
    cache = a->scan_cache;
    if (cache->count >= IWM_SCAN_MAX_RESULTS)
        return IWM_EOVERFLOW;
    dst = &cache->result[cache->count++];
    memcpy(dst, result, sizeof(*dst));
    return IWM_OK;
}

void iwm_genmac_scan_complete(struct iwm_adapter *a, int status)
{
    if (!a)
        return;
    if (status == IWM_OK) {
        a->state = IWM_ADAPTER_READY;
        ++a->status_generation;
    } else {
        a->state = IWM_ADAPTER_ERROR;
    }
}
