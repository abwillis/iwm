/* SPDX-License-Identifier: GPL-2.0-only */
/* Ring 0: Dev32lib.h (forced in with -fi) declares memcpy/memset and the
 * miniddk string.h conflicts with it. */
#if !defined(IWM_RING0)
#include <string.h>
#endif
#include "iwm_hw.h"
#include "iwm_genmac.h"

#define CSR_HW_IF_CONFIG_REG 0x000UL
#define CSR_INT              0x008UL
#define CSR_INT_MASK         0x00cUL
#define CSR_FH_INT_STATUS    0x010UL
#define CSR_RESET            0x020UL
#define CSR_GP_CNTRL         0x024UL
#define CSR_HW_REV           0x028UL
#define CSR_MBOX_SET_REG     0x088UL
#define CSR_GIO_CHICKEN_BITS 0x100UL
#define CSR_DBG_HPET_MEM_REG 0x240UL
#define CSR_DBG_LINK_PWR_MGMT_REG 0x250UL
#define CSR_GIO_REG          0x03cUL
#define CSR_GIO_REG_VAL_L0S_DISABLED 0x00000002UL
#define HBUS_TARG_PRPH_WADDR 0x444UL
#define HBUS_TARG_PRPH_RADDR 0x448UL
#define HBUS_TARG_PRPH_WDAT  0x44cUL
#define HBUS_TARG_PRPH_RDAT  0x450UL
#define HBUS_PRPH_DWORD      (3UL << 24)
/* Linux iwl_trans_pcie_clear_persistence_bit() (iwl-prph.h). */
#define HPM_DEBUG            0x00a03440UL
#define HPM_PERSISTENCE_BIT  0x00001000UL
#define PREG_PRPH_WPROT_9000 0x00a04ce0UL
#define PREG_WFPM_ACCESS     0x00001000UL
/* FreeBSD iwm_attach() (family >= 8000) sets ENABLE_WFPM in WFPM_CTRL_REG;
 * Linux _iwl_pci_probe() calls get_crf_id() under NIC access at the same
 * point. */
#define WFPM_CTRL_REG        0x00a03030UL
#define ENABLE_WFPM          0x80000000UL
/* Linux iwl_pcie_conf_msix_hw() / OpenBSD iwm_conf_msix_hw(): MQ-RX parts
 * default to MSI-X; a driver using plain MSI must select it in UREG_CHICK
 * once the device is enabled (after APM init). */
#define UREG_CHICK           0x00a05c00UL
#define UREG_CHICK_MSI_ENABLE 0x01000000UL

/* PREPARE (Linux CSR_HW_IF_CONFIG_REG_WAKE_ME, 0x08000000) is host-owned;
 * NIC_READY (Linux CSR_HW_IF_CONFIG_REG_PCI_OWN_SET, 0x00400000) is set by
 * the device and only observed by the driver. */
#define CSR_HW_IF_NIC_READY  0x00400000UL
#define CSR_HW_IF_PREPARE    0x08000000UL
#define CSR_HW_IF_HAP_WAKE_L1A 0x00080000UL
#define CSR_GIO_L1A_NO_L0S_RX 0x00800000UL
#define CSR_GIO_DIS_L0S_EXIT_TIMER 0x20000000UL
#define CSR_DBG_HPET_MAX_WAIT 0xffff0000UL
#define CSR_LINK_PWR_MGMT_DISABLED 0x80000000UL
#define CSR_RESET_SW         0x00000080UL
#define CSR_RESET_MASTER_DISABLED 0x00000100UL
#define CSR_RESET_STOP_MASTER 0x00000200UL
#define CSR_GP_CLOCK_READY   0x00000001UL
#define CSR_GP_INIT_DONE     0x00000004UL
#define CSR_GP_ACCESS_REQ    0x00000008UL
#define CSR_GP_GOING_SLEEP   0x00000010UL
#define CSR_MBOX_OS_ALIVE    0x00000020UL

static uint32_t rd(struct iwm_adapter *a, uint32_t reg)
{
    return a->ops->mmio_read32(a->os_context, reg);
}

static void wr(struct iwm_adapter *a, uint32_t reg, uint32_t value)
{
    a->ops->mmio_write32(a->os_context, reg, value);
}

static int poll(struct iwm_adapter *a, uint32_t reg, uint32_t mask,
                uint32_t expected, uint32_t loops, uint32_t delay_us)
{
    while (loops--) {
        if ((rd(a, reg) & mask) == expected)
            return IWM_OK;
        a->ops->delay_us(a->os_context, delay_us);
    }
    return IWM_ETIMEDOUT;
}

void iwm_adapter_init(struct iwm_adapter *a,
                      const struct iwm_os_ops *ops, void *context)
{
    memset(a, 0, sizeof(*a));
    a->ops = ops;
    a->os_context = context;
    a->state = IWM_ADAPTER_DOWN;
    a->rssi_dbm = -200;
    a->infrastructure_mode = IWM_INFRASTRUCTURE_BSS;
    a->authentication_mode = IWM_AUTH_AUTO;
    a->encryption_status = IWM_ENCRYPTION_DISABLED;
    a->privacy_filter = IWM_PRIVACY_ACCEPT_ALL;
    a->power_mode = 0; /* Ndis802_11PowerModeCAM */
    a->network_type = IWM_NETWORK_TYPE_OFDM24;
    iwm_fw_session_init(&a->firmware, NULL);
}

int iwm_adapter_set_fw_store(struct iwm_adapter *a, struct iwm_blob *store)
{
    if (!a)
        return IWM_EINVAL;
    if (!store) {
        iwm_fw_session_init(&a->firmware, NULL);
        return IWM_OK;
    }
    if (a->firmware.state != IWM_FW_EMPTY &&
        a->firmware.state != IWM_FW_FAILED)
        return IWM_EBUSY;
    iwm_fw_session_init(&a->firmware, store);
    return IWM_OK;
}

int iwm_adapter_set_mac(struct iwm_adapter *a, const uint8_t mac[6])
{
    unsigned i;
    uint8_t any = 0;
    if (!a || !mac || (mac[0] & 1U))
        return IWM_EINVAL;
    for (i = 0; i < 6; ++i)
        any |= mac[i];
    if (!any)
        return IWM_EINVAL;
    memcpy(a->mac, mac, sizeof(a->mac));
    a->mac_valid = 1;
    return IWM_OK;
}

/* Linux iwl_flip_hw_address(): the first register holds bytes 0-3 most
 * significant first, the second holds bytes 4-5 in its low half. */
static void flip_hw_address(uint32_t r0, uint32_t r1, uint8_t mac[6])
{
    mac[0] = (uint8_t)(r0 >> 24);
    mac[1] = (uint8_t)(r0 >> 16);
    mac[2] = (uint8_t)(r0 >> 8);
    mac[3] = (uint8_t)r0;
    mac[4] = (uint8_t)(r1 >> 8);
    mac[5] = (uint8_t)r1;
}

int iwm_adapter_read_csr_mac(struct iwm_adapter *a, uint32_t base,
                             uint32_t raw[4])
{
    uint32_t v[4];
    uint8_t mac[6];

    if (!a || !a->ops || !a->ops->mmio_read32 || base == 0 ||
        base > 0x3FF0UL)
        return IWM_EINVAL;
    v[0] = rd(a, base + 0x08UL);   /* MAC_ADDR0_STRAP */
    v[1] = rd(a, base + 0x0CUL);   /* MAC_ADDR1_STRAP */
    v[2] = rd(a, base + 0x00UL);   /* MAC_ADDR0_OTP */
    v[3] = rd(a, base + 0x04UL);   /* MAC_ADDR1_OTP */
    if (raw)
        memcpy(raw, v, sizeof(v));
    flip_hw_address(v[0], v[1], mac);
    if (iwm_adapter_set_mac(a, mac) == IWM_OK)
        return IWM_OK;
    flip_hw_address(v[2], v[3], mac);
    if (iwm_adapter_set_mac(a, mac) == IWM_OK)
        return IWM_OK;
    return IWM_ENODEV;
}

int iwm_adapter_probe(struct iwm_adapter *a, uint16_t vendor,
                      uint16_t device, uint32_t bar0, uint8_t irq)
{
    if (!a || !a->ops || !a->ops->mmio_read32 || !a->ops->mmio_write32 ||
        !a->ops->delay_us)
        return IWM_EINVAL;
    a->cfg = iwm_lookup_device(vendor, device);
    if (!a->cfg)
        return IWM_ENODEV;
    a->bar0 = bar0;
    a->irq = irq;
    a->hw_rev = rd(a, CSR_HW_REV);
    if (a->hw_rev == 0xFFFFFFFFUL)
        return IWM_ENODEV;
    a->state = IWM_ADAPTER_PROBED;
    iwm_plan_rings(a, &a->ring_plan);
    return IWM_OK;
}

/* PRPH access without NIC-access arbitration (Linux *_no_grab), used before
 * APM init when MAC_ACCESS_REQ cannot be honoured yet. */
static uint32_t prph_rd_nograb(struct iwm_adapter *a, uint32_t reg)
{
    wr(a, HBUS_TARG_PRPH_RADDR, reg | HBUS_PRPH_DWORD);
    return rd(a, HBUS_TARG_PRPH_RDAT);
}
static void prph_wr_nograb(struct iwm_adapter *a, uint32_t reg, uint32_t v)
{
    wr(a, HBUS_TARG_PRPH_WADDR, reg | HBUS_PRPH_DWORD);
    wr(a, HBUS_TARG_PRPH_WDAT, v);
}
static int hw_error_value(uint32_t v)
{
    return v == 0xFFFFFFFFUL || (v & ~0xFUL) == 0xA5A5A5A0UL ||
           (v & ~0xFUL) == 0x5A5A5A50UL;
}
/* Linux iwl_trans_pcie_clear_persistence_bit(): while PERSISTENCE is set the
 * device keeps its state across a software reset (state left by another OS
 * or by firmware), so the secure-boot ROM does not start from reset. */
static int clear_persistence(struct iwm_adapter *a)
{
    uint32_t hpm;
    if (!a->cfg || a->cfg->family != IWM_FAMILY_9000)
        return IWM_OK;
    hpm = prph_rd_nograb(a, HPM_DEBUG);
    a->hpm_debug = hpm;
    if (hw_error_value(hpm) || !(hpm & HPM_PERSISTENCE_BIT))
        return IWM_OK;
    if (prph_rd_nograb(a, PREG_PRPH_WPROT_9000) & PREG_WFPM_ACCESS)
        return IWM_EIO; /* Linux: "can not clear persistence bit" */
    prph_wr_nograb(a, HPM_DEBUG, hpm & ~HPM_PERSISTENCE_BIT);
    a->hpm_after = prph_rd_nograb(a, HPM_DEBUG);
    return IWM_OK;
}
/* FreeBSD iwm_attach() runs this once before any start_hw: ownership,
 * INIT_DONE, wait for the MAC clock, then under NIC access set ENABLE_WFPM.
 * The device stays in D0A, so the HPM_DEBUG read of the persistence check
 * that follows is meaningful. Without it the boot log read 0xa5a5a5a0 there,
 * which the check treats as "nothing to clear". Define IWM_NO_WFPM to compile
 * it out. */
static int wake_and_enable_wfpm(struct iwm_adapter *a)
{
#ifdef IWM_NO_WFPM
    (void)a;
    return IWM_OK;
#else
    uint32_t v;
    int rc;

    wr(a, CSR_GP_CNTRL, rd(a, CSR_GP_CNTRL) | CSR_GP_INIT_DONE);
    a->ops->delay_us(a->os_context, 2);
    rc = poll(a, CSR_GP_CNTRL, CSR_GP_CLOCK_READY, CSR_GP_CLOCK_READY,
              250, 100);
    if (rc == IWM_OK)
        rc = iwm_grab_nic_access(a);
    if (rc == IWM_OK) {
        v = prph_rd_nograb(a, WFPM_CTRL_REG);
        a->wfpm_ctrl[0] = v;
        prph_wr_nograb(a, WFPM_CTRL_REG, v | ENABLE_WFPM);
        a->wfpm_ctrl[1] = prph_rd_nograb(a, WFPM_CTRL_REG);
        iwm_release_nic_access(a);
    }
    a->wfpm_rc = rc;
    return rc;
#endif
}
static int g_in_sw_reset;
int iwm_prepare_card_hw(struct iwm_adapter *a)
{
    uint32_t v;
    uint32_t elapsed;
    int rc;
    if (!a || a->state < IWM_ADAPTER_PROBED)
        return IWM_ESTATE;

    /* Ask for PCI ownership first.  If AMT/firmware has not released the
     * device yet, use the documented PREPARE fallback and retry for 150 ms. */
    v = rd(a, CSR_HW_IF_CONFIG_REG) | CSR_HW_IF_NIC_READY;
    wr(a, CSR_HW_IF_CONFIG_REG, v);
    rc = poll(a, CSR_HW_IF_CONFIG_REG, CSR_HW_IF_NIC_READY,
              CSR_HW_IF_NIC_READY, 50, 200);
    if (rc != IWM_OK) {
        wr(a, CSR_DBG_LINK_PWR_MGMT_REG,
           rd(a, CSR_DBG_LINK_PWR_MGMT_REG) |
           CSR_LINK_PWR_MGMT_DISABLED);
        a->ops->delay_us(a->os_context, 1000);
        wr(a, CSR_HW_IF_CONFIG_REG,
           rd(a, CSR_HW_IF_CONFIG_REG) | CSR_HW_IF_PREPARE);
        for (elapsed = 0; elapsed < 150000UL; elapsed += 200U) {
            wr(a, CSR_HW_IF_CONFIG_REG,
               rd(a, CSR_HW_IF_CONFIG_REG) | CSR_HW_IF_NIC_READY);
            if (rd(a, CSR_HW_IF_CONFIG_REG) & CSR_HW_IF_NIC_READY) {
                rc = IWM_OK;
                break;
            }
            a->ops->delay_us(a->os_context, 200);
        }
        if (rc != IWM_OK)
            return rc;
    }
    wr(a, CSR_MBOX_SET_REG, CSR_MBOX_OS_ALIVE);
    /* Linux _iwl_trans_pcie_start_hw(): ownership, persistence clear, software
     * reset (CPUs stay in reset until RELEASE_CPU_RESET), then ownership again
     * before APM init. */
    if (!g_in_sw_reset) {
        /* Failure is logged through wfpm_rc; the reset path continues. */
        (void)wake_and_enable_wfpm(a);
        rc = clear_persistence(a);
        if (rc != IWM_OK)
            return rc;
        wr(a, CSR_RESET, rd(a, CSR_RESET) | CSR_RESET_SW);
        a->ops->delay_us(a->os_context, 6000);
        g_in_sw_reset = 1;
        rc = iwm_prepare_card_hw(a);
        g_in_sw_reset = 0;
        return rc;
    }

    /* APM bring-up needed before FH/PRPH access. */
    v = rd(a, CSR_GIO_CHICKEN_BITS) | CSR_GIO_L1A_NO_L0S_RX;
    if (a->cfg && a->cfg->family < IWM_FAMILY_8000)
        v |= CSR_GIO_DIS_L0S_EXIT_TIMER;
    wr(a, CSR_GIO_CHICKEN_BITS, v);
    wr(a, CSR_DBG_HPET_MEM_REG,
       rd(a, CSR_DBG_HPET_MEM_REG) | CSR_DBG_HPET_MAX_WAIT);
    wr(a, CSR_HW_IF_CONFIG_REG,
       rd(a, CSR_HW_IF_CONFIG_REG) | CSR_HW_IF_HAP_WAKE_L1A);
    /* Linux iwl_pcie_apm_config(): L0s is always disabled. */
    wr(a, CSR_GIO_REG, rd(a, CSR_GIO_REG) | CSR_GIO_REG_VAL_L0S_DISABLED);
    v = rd(a, CSR_GP_CNTRL) | CSR_GP_INIT_DONE;
    wr(a, CSR_GP_CNTRL, v);
    rc = poll(a, CSR_GP_CNTRL, CSR_GP_CLOCK_READY,
              CSR_GP_CLOCK_READY, 250, 100);
    if (rc == IWM_OK && a->cfg && a->cfg->mq_rx) {
        a->ureg_chick[0] = 0xFFFFFFFFUL;
        a->ureg_chick[1] = 0xFFFFFFFFUL;
        rc = iwm_grab_nic_access(a);
        if (rc == IWM_OK) {
            a->ureg_chick[0] = prph_rd_nograb(a, UREG_CHICK);
            prph_wr_nograb(a, UREG_CHICK, UREG_CHICK_MSI_ENABLE);
            a->ureg_chick[1] = prph_rd_nograb(a, UREG_CHICK);
            iwm_release_nic_access(a);
        }
    }
    if (rc == IWM_OK)
        a->state = IWM_ADAPTER_FW_WAIT;
    return rc;
}

int iwm_grab_nic_access(struct iwm_adapter *a)
{
    uint32_t v;
    if (!a || a->state < IWM_ADAPTER_PROBED)
        return IWM_ESTATE;
    v = rd(a, CSR_GP_CNTRL) | CSR_GP_ACCESS_REQ;
    wr(a, CSR_GP_CNTRL, v);
    /* FreeBSD iwm_nic_lock(): on 8000 and later, 2 us for the MAC to drop a
     * stale CLOCK_READY before it is polled. */
    if (a->cfg && a->cfg->family >= IWM_FAMILY_8000)
        a->ops->delay_us(a->os_context, 2);
    return poll(a, CSR_GP_CNTRL, CSR_GP_CLOCK_READY | CSR_GP_GOING_SLEEP,
                CSR_GP_CLOCK_READY, 150, 100);
}

void iwm_release_nic_access(struct iwm_adapter *a)
{
    if (a)
        wr(a, CSR_GP_CNTRL, rd(a, CSR_GP_CNTRL) & ~CSR_GP_ACCESS_REQ);
}

int iwm_stop_device(struct iwm_adapter *a)
{
    uint32_t v;
    if (!a || a->state == IWM_ADAPTER_DOWN)
        return IWM_ESTATE;
    wr(a, CSR_INT_MASK, 0);
    wr(a, CSR_INT, 0xFFFFFFFFUL);
    wr(a, CSR_FH_INT_STATUS, 0xFFFFFFFFUL);
    v = rd(a, CSR_RESET) | CSR_RESET_STOP_MASTER;
    wr(a, CSR_RESET, v);
    (void)poll(a, CSR_RESET, CSR_RESET_MASTER_DISABLED,
               CSR_RESET_MASTER_DISABLED, 100, 10);
    wr(a, CSR_RESET, rd(a, CSR_RESET) | CSR_RESET_SW);
    a->state = IWM_ADAPTER_DOWN;
    /* Firmware is gone after SW reset; permit a fresh BEGIN. */
    iwm_fw_session_init(&a->firmware, a->firmware.store);
    return IWM_OK;
}

int iwm_adapter_fw_alive(struct iwm_adapter *a)
{
    int rc;
    if (!a)
        return IWM_EINVAL;
    rc = iwm_fw_mark_running(&a->firmware);
    if (rc != IWM_OK)
        return rc;
    a->state = IWM_ADAPTER_READY;
    ++a->status_generation;
    return IWM_OK;
}



