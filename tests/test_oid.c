/* SPDX-License-Identifier: GPL-2.0-only */
/* Unimplemented security operations must not report success. */
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

int main(void)
{
    struct iwm_adapter a;
    struct iwm_os_ops ops;
    struct iwm_ndis_context n;
    struct iwm_oid_request r;
    uint32_t value;
    int cookie = 0;

    memset(&ops, 0, sizeof(ops));
    ops.mmio_read32 = rd;
    ops.mmio_write32 = wr;
    ops.delay_us = dl;
    regs[0x28 >> 2] = 0x1234;
    iwm_adapter_init(&a, &ops, NULL);
    assert(iwm_adapter_probe(&a, 0x8086, 0x9df0, 0xF0000000UL, 11) == IWM_OK);

    assert(set_oid(&a, OID_802_11_BSSID_LIST_SCAN) == IWM_ESTATE);
    assert(set_oid(&a, OID_802_11_AUTHENTICATION_MODE) == IWM_ENOTSUP);
    assert(set_oid(&a, OID_802_11_ENCRYPTION_STATUS) == IWM_ENOTSUP);
    assert(set_oid(&a, OID_802_11_PRIVACY_FILTER) == IWM_ENOTSUP);
    assert(set_oid(&a, OID_802_11_ADD_KEY) == IWM_ENOTSUP);
    assert(set_oid(&a, OID_802_11_REMOVE_KEY) == IWM_ENOTSUP);
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

    /* TX is gated on association and still unimplemented after it. */
    memset(&n, 0, sizeof(n));
    n.adapter = &a;
    assert(iwm_ndis_transmit(&n, &cookie, 1, &cookie) == IWM_ESTATE);
    a.state = IWM_ADAPTER_ASSOCIATED;
    assert(iwm_ndis_transmit(&n, &cookie, 1, &cookie) == IWM_ENOTSUP);

    puts("oid tests passed");
    return 0;
}


