/* SPDX-License-Identifier: GPL-2.0-only */
/* GenMAC policy, address and scan-list behavior from the XWLAN trace. */
#include "assert.h"
#include "stdio.h"
#include "string.h"
#include "iwm_ndis.h"
#include "iwm_genmac.h"

static uint32_t regs[64];
static uint32_t rd(void *c, uint32_t o) { (void)c; return regs[(o >> 2) & 63]; }
static void wr(void *c, uint32_t o, uint32_t v) { (void)c; regs[(o >> 2) & 63] = v; }
static void dl(void *c, uint32_t u) { (void)c; (void)u; }

static int set_oid(struct iwm_adapter *a, uint32_t id)
{
    struct iwm_oid_request r;
    uint32_t scratch = 0;
    memset(&r, 0, sizeof(r));
    r.oid = id;
    r.data = &scratch;
    r.data_length = sizeof(scratch);
    return iwm_genmac_oid_set(a, &r);
}

static int set_oid_value(struct iwm_adapter *a, uint32_t id, uint32_t value,
                         struct iwm_oid_request *out)
{
    struct iwm_oid_request r;
    memset(&r, 0, sizeof(r));
    r.oid = id;
    r.data = &value;
    r.data_length = sizeof(value);
    if (out)
        *out = r;
    return iwm_genmac_oid_set(a, out ? out : &r);
}

static int get_oid_u32(struct iwm_adapter *a, uint32_t id, uint32_t *value)
{
    struct iwm_oid_request r;
    memset(&r, 0, sizeof(r));
    r.oid = id;
    r.data = value;
    r.data_length = sizeof(*value);
    return iwm_genmac_oid_get(a, &r);
}

static int get_oid(struct iwm_adapter *a, uint32_t id, void *data,
                   uint32_t length, struct iwm_oid_request *out)
{
    struct iwm_oid_request r;
    memset(&r, 0, sizeof(r));
    r.oid = id;
    r.data = data;
    r.data_length = length;
    if (out)
        *out = r;
    return iwm_genmac_oid_get(a, out ? out : &r);
}

int main(void)
{
    struct iwm_adapter a;
    struct iwm_os_ops ops;
    struct iwm_ndis_context n;
    struct iwm_oid_request r;
    struct iwm_scan_cache cache;
    struct iwm_scan_result scan;
    uint8_t list[512];
    uint8_t mac[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
    uint8_t got_mac[6];
    uint32_t value;
    int cookie = 0;

    memset(&ops, 0, sizeof(ops));
    ops.mmio_read32 = rd;
    ops.mmio_write32 = wr;
    ops.delay_us = dl;
    regs[0x28 >> 2] = 0x1234;
    iwm_adapter_init(&a, &ops, NULL);
    assert(iwm_adapter_probe(&a, 0x8086, 0x9df0, 0xF0000000UL, 11) == IWM_OK);
    iwm_genmac_bind_scan_cache(&a, &cache);

    assert(set_oid(&a, OID_802_11_BSSID_LIST_SCAN) == IWM_ESTATE);
    assert(set_oid_value(&a, OID_802_11_AUTHENTICATION_MODE,
                         IWM_AUTH_AUTO, NULL) == IWM_OK);
    assert(set_oid_value(&a, OID_802_11_ENCRYPTION_STATUS,
                         IWM_ENCRYPTION_DISABLED, NULL) == IWM_OK);
    assert(set_oid_value(&a, OID_802_11_PRIVACY_FILTER,
                         IWM_PRIVACY_ACCEPT_ALL, NULL) == IWM_OK);
    assert(set_oid(&a, OID_802_11_ADD_KEY) == IWM_ENOTSUP);
    assert(set_oid(&a, OID_802_11_REMOVE_KEY) == IWM_ENOTSUP);
    /* Open-network connect sequence from XWLAN: REMOVE_WEP for each key
     * index must succeed; installing a WEP key stays unsupported. */
    {
        struct iwm_oid_request rw;
        uint16_t shortdata = 3;
        assert(set_oid_value(&a, OID_802_11_REMOVE_WEP, 0, &rw) == IWM_OK);
        assert(rw.bytes_done == 4U);
        assert(set_oid_value(&a, OID_802_11_REMOVE_WEP, 3, NULL) == IWM_OK);
        assert(set_oid_value(&a, OID_802_11_REMOVE_WEP, 0x80000001UL, NULL) == IWM_OK);
        memset(&rw, 0, sizeof(rw));
        rw.oid = OID_802_11_REMOVE_WEP;
        rw.data = &shortdata;
        rw.data_length = sizeof(shortdata);
        assert(iwm_genmac_oid_set(&a, &rw) == IWM_EINVAL);
        rw.data = NULL;
        rw.data_length = 0;
        assert(iwm_genmac_oid_set(&a, &rw) == IWM_EINVAL);
        assert(set_oid(&a, OID_802_11_ADD_WEP) == IWM_ENOTSUP);
    }
    assert(set_oid(&a, OID_802_11_INFRASTRUCTURE_MODE) == IWM_OK);

    value = 99;
    assert(get_oid_u32(&a, OID_GEN_HARDWARE_STATUS, &value) == IWM_OK);
    assert(value == IWM_NDIS_HARDWARE_READY);
    value = 99;
    assert(get_oid_u32(&a, OID_802_11_INFRASTRUCTURE_MODE, &value) == IWM_OK);
    assert(value == IWM_INFRASTRUCTURE_IBSS);
    memset(&r, 0, sizeof(r));
    assert(set_oid_value(&a, OID_802_11_INFRASTRUCTURE_MODE,
                         IWM_INFRASTRUCTURE_BSS, &r) == IWM_OK);
    assert(r.bytes_done == sizeof(value));
    value = 99;
    assert(get_oid_u32(&a, OID_802_11_INFRASTRUCTURE_MODE, &value) == IWM_OK);
    assert(value == IWM_INFRASTRUCTURE_BSS);
    assert(set_oid_value(&a, OID_802_11_INFRASTRUCTURE_MODE, 3, NULL) ==
           IWM_EINVAL);

    value = 99;
    assert(get_oid_u32(&a, OID_802_11_ENCRYPTION_STATUS, &value) == IWM_OK);
    assert(value == IWM_ENCRYPTION_DISABLED);

    memset(&r, 0, sizeof(r));
    memset(got_mac, 0, sizeof(got_mac));
    assert(get_oid(&a, OID_802_3_CURRENT_ADDRESS, got_mac,
                   sizeof(got_mac), &r) == IWM_ESTATE);
    assert(r.ndis_status == IWM_NDIS_STATUS_ADAPTER_NOT_READY);
    assert(iwm_adapter_set_mac(&a, mac) == IWM_OK);
    assert(get_oid(&a, OID_802_3_CURRENT_ADDRESS, got_mac,
                   sizeof(got_mac), &r) == IWM_OK);
    assert(memcmp(got_mac, mac, sizeof(mac)) == 0);

    memset(&scan, 0, sizeof(scan));
    memcpy(scan.bssid, "\x00\x16\x3e\x12\x34\x56", 6);
    memcpy(scan.ssid, "test-ap", 7);
    scan.ssid_length = 7;
    scan.rssi_dbm = -52;
    scan.ds_config_khz = 2412000UL;
    scan.beacon_period = 100;
    scan.infrastructure_mode = IWM_INFRASTRUCTURE_BSS;
    scan.network_type = IWM_NETWORK_TYPE_OFDM24;
    scan.rates[0] = 0x82;
    assert(iwm_genmac_scan_add(&a, &scan) == IWM_OK);
    memset(list, 0, sizeof(list));
    assert(get_oid(&a, OID_802_11_BSSID_LIST, list,
                   sizeof(list), &r) == IWM_OK);
    memcpy(&value, list, sizeof(value));
    assert(value == 1);
    memcpy(&value, list + 4, sizeof(value));
    assert(value == 116);
    assert(memcmp(list + 8, scan.bssid, 6) == 0);

    /* TX is gated on association and still unimplemented after it. */
    memset(&n, 0, sizeof(n));
    n.adapter = &a;
    assert(iwm_ndis_transmit(&n, &cookie, 1, &cookie) == IWM_ESTATE);
    a.state = IWM_ADAPTER_ASSOCIATED;
    assert(iwm_ndis_transmit(&n, &cookie, 1, &cookie) == IWM_ENOTSUP);

    puts("oid tests passed");
    return 0;
}



