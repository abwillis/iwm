/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef IWM_GENMAC_H
#define IWM_GENMAC_H

#include "iwm_hw.h"

#define GENMAC_CATEGORY              0x99U
#define GENMAC_WRAPPER_OID_GET       0x40U
#define GENMAC_WRAPPER_OID_SET       0x41U

#define OID_GEN_LINK_SPEED                  0x00010107UL
#define OID_GEN_MEDIA_CONNECT_STATUS        0x00010114UL
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
#define IWM_MEDIA_CONNECTED 0UL
#define IWM_MEDIA_DISCONNECTED 1UL

struct IWM_PACKED iwm_ndis_ssid {
    uint32_t length;
    uint8_t ssid[32];
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

#endif
