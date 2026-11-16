/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef IWM_GENMAC_H
#define IWM_GENMAC_H

#include "iwm_hw.h"

#define GENMAC_CATEGORY              0x99U
#define GENMAC_WRAPPER_OID_GET       0x40U
#define GENMAC_WRAPPER_OID_SET       0x41U

#define OID_GEN_HARDWARE_STATUS             0x00010102UL
#define OID_GEN_LINK_SPEED                  0x00010107UL
#define OID_GEN_MEDIA_CONNECT_STATUS        0x00010114UL
#define OID_802_3_PERMANENT_ADDRESS         0x01010101UL
#define OID_802_3_CURRENT_ADDRESS           0x01010102UL
#define OID_802_11_BSSID                    0x0D010101UL
#define OID_802_11_SSID                     0x0D010102UL
#define OID_802_11_NETWORK_TYPE_IN_USE      0x0D010204UL
#define OID_802_11_RSSI                     0x0D010206UL
#define OID_802_11_INFRASTRUCTURE_MODE      0x0D010108UL
#define OID_802_11_DISASSOCIATE             0x0D010115UL
#define OID_802_11_POWER_MODE               0x0D010216UL
#define OID_802_11_BSSID_LIST               0x0D010217UL
#define OID_802_11_AUTHENTICATION_MODE      0x0D010118UL
#define OID_802_11_PRIVACY_FILTER           0x0D010119UL
#define OID_802_11_BSSID_LIST_SCAN          0x0D01011AUL
#define OID_802_11_ENCRYPTION_STATUS        0x0D01011BUL
#define OID_802_11_ADD_KEY                  0x0D01011DUL
#define OID_802_11_REMOVE_KEY               0x0D01011EUL

#define OID_PRIVATE_WRAPPER_LANNUMBER        0xFFFFFF00UL
#define OID_PRIVATE_WRAPPER_ISWIRELESS       0xFFFFFF01UL
#define OID_PRIVATE_WRAPPER_LINKSTATUS       0xFFFFFF02UL
#define OID_PRIVATE_WRAPPER_STATUSCHANGE     0xFFFFFF03UL
#define OID_PRIVATE_WRAPPER_HALT             0xFFFFFF04UL
#define OID_PRIVATE_WRAPPER_REINIT           0xFFFFFF05UL
#define OID_PRIVATE_WRAPPER_WINDRIVER_NIFNAME 0xFFFFFF08UL
#define OID_PRIVATE_WRAPPER_GENMAC_VERSION   0xFFFFFF09UL

#define IWM_GENMAC_VERSION 0x00030000UL
#define IWM_NDIS_HARDWARE_READY 0UL
#define IWM_MEDIA_CONNECTED 0UL
#define IWM_MEDIA_DISCONNECTED 1UL

#define IWM_INFRASTRUCTURE_IBSS 0UL
#define IWM_INFRASTRUCTURE_BSS  1UL
#define IWM_INFRASTRUCTURE_AUTO 2UL

#define IWM_NDIS_STATUS_NOT_SUPPORTED     0xC00000BBUL
#define IWM_NDIS_STATUS_ADAPTER_NOT_READY 0xC0010011UL

#define IWM_AUTH_OPEN          0UL
#define IWM_AUTH_SHARED        1UL
#define IWM_AUTH_AUTO          2UL
#define IWM_AUTH_WPA           3UL
#define IWM_AUTH_WPA_PSK       4UL
#define IWM_AUTH_WPA_NONE      5UL
#define IWM_AUTH_MAX           6UL

#define IWM_PRIVACY_ACCEPT_ALL 0UL
#define IWM_PRIVACY_8021X_WEP  1UL

#define IWM_ENCRYPTION_WEP_ENABLED       0UL
#define IWM_ENCRYPTION_DISABLED          1UL
#define IWM_ENCRYPTION_WEP_KEY_ABSENT    2UL
#define IWM_ENCRYPTION_NOT_SUPPORTED     3UL
#define IWM_ENCRYPTION_TKIP_ENABLED      4UL
#define IWM_ENCRYPTION_TKIP_KEY_ABSENT   5UL
#define IWM_ENCRYPTION_AES_ENABLED       6UL
#define IWM_ENCRYPTION_AES_KEY_ABSENT    7UL

#define IWM_NETWORK_TYPE_FH      0UL
#define IWM_NETWORK_TYPE_DS      1UL
#define IWM_NETWORK_TYPE_OFDM5   2UL
#define IWM_NETWORK_TYPE_OFDM24  3UL

#define IWM_SCAN_MAX_RESULTS 32U
#define IWM_SCAN_MAX_IE      256U

struct IWM_PACKED iwm_ndis_ssid {
    uint32_t length;
    uint8_t ssid[32];
};

/* Portable representation of one firmware scan result.  The GenMAC layer
 * serializes this to NDIS_WLAN_BSSID_EX only at the IOCTL boundary. */
struct iwm_scan_result {
    uint8_t bssid[6];
    uint8_t ssid[32];
    uint8_t ssid_length;
    uint8_t privacy;
    uint8_t infrastructure_mode;
    uint8_t network_type;
    int32_t rssi_dbm;
    uint32_t ds_config_khz;
    uint32_t beacon_period;
    uint8_t rates[16];
    uint16_t ie_length;
    uint8_t ies[IWM_SCAN_MAX_IE];
};

struct iwm_scan_cache {
    uint16_t count;
    uint16_t reserved;
    uint32_t generation;
    struct iwm_scan_result result[IWM_SCAN_MAX_RESULTS];
};

struct iwm_oid_request {
    uint32_t oid;
    void *data;
    uint32_t data_length;
    uint32_t bytes_done;
    uint32_t ndis_status;
};

int iwm_genmac_oid_get(struct iwm_adapter *adapter,
                       struct iwm_oid_request *request);
int iwm_genmac_oid_set(struct iwm_adapter *adapter,
                       struct iwm_oid_request *request);
void iwm_genmac_bind_scan_cache(struct iwm_adapter *adapter,
                                struct iwm_scan_cache *cache);
void iwm_genmac_scan_reset(struct iwm_adapter *adapter);
int iwm_genmac_scan_add(struct iwm_adapter *adapter,
                        const struct iwm_scan_result *result);
void iwm_genmac_scan_complete(struct iwm_adapter *adapter, int status);

#endif



