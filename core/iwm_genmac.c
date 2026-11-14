/* SPDX-License-Identifier: GPL-2.0-only */
#include <string.h>
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

int iwm_genmac_oid_get(struct iwm_adapter *a, struct iwm_oid_request *r)
{
    struct iwm_ndis_ssid ssid;
    uint32_t value;
    if (!a || !r)
        return IWM_EINVAL;
    switch (r->oid) {
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
        value = 0; /* CAM/no power save for the first milestone. */
        return put_u32(r, value);
    default:
        r->ndis_status = 0xC00000BBUL; /* NDIS_STATUS_NOT_SUPPORTED */
        return IWM_ENOTSUP;
    }
}

int iwm_genmac_oid_set(struct iwm_adapter *a, struct iwm_oid_request *r)
{
    const struct iwm_ndis_ssid *ssid;
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
    case OID_802_11_POWER_MODE:
        /* Policy values with no hardware effect before association. */
        r->bytes_done = r->data_length;
        return IWM_OK;
    case OID_802_11_AUTHENTICATION_MODE:
    case OID_802_11_ENCRYPTION_STATUS:
    case OID_802_11_PRIVACY_FILTER:
        /* Security state must not be reported as applied until the firmware
         * station and key commands exist. */
        r->ndis_status = 0xC00000BBUL;
        return IWM_ENOTSUP;
    case OID_802_11_ADD_KEY:
    case OID_802_11_REMOVE_KEY:
        /* Key-command construction is a required milestone before WPA traffic. */
        r->ndis_status = 0xC00000BBUL;
        return IWM_ENOTSUP;
    default:
        r->ndis_status = 0xC00000BBUL;
        return IWM_ENOTSUP;
    }
}
