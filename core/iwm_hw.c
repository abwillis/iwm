/* SPDX-License-Identifier: GPL-2.0-only */
/* Ring 0: Dev32lib.h (forced in with -fi) declares memcpy/memset and the
 * miniddk string.h conflicts with it. */
#if !defined(IWM_RING0)
#include <string.h>
#endif
#include "iwm_hw.h"

#define CSR_HW_IF_CONFIG_REG 0x000UL
#define CSR_INT              0x008UL
#define CSR_INT_MASK         0x00cUL
#define CSR_FH_INT_STATUS    0x010UL
#define CSR_RESET            0x020UL
#define CSR_GP_CNTRL         0x024UL
#define CSR_HW_REV           0x028UL
#define CSR_MBOX_SET_REG     0x088UL

/* PREPARE (Linux CSR_HW_IF_CONFIG_REG_WAKE_ME, 0x08000000) is host-owned;
 * NIC_READY (Linux CSR_HW_IF_CONFIG_REG_PCI_OWN_SET, 0x00400000) is set by
 * the device and only observed by the driver. */
#define CSR_HW_IF_NIC_READY  0x00400000UL
#define CSR_HW_IF_PREPARE    0x08000000UL
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

int iwm_prepare_card_hw(struct iwm_adapter *a)
{
    uint32_t v;
    int rc;
    if (!a || a->state < IWM_ADAPTER_PROBED)
        return IWM_ESTATE;
    v = rd(a, CSR_HW_IF_CONFIG_REG) | CSR_HW_IF_PREPARE;
    wr(a, CSR_HW_IF_CONFIG_REG, v);
    rc = poll(a, CSR_HW_IF_CONFIG_REG, CSR_HW_IF_NIC_READY,
              CSR_HW_IF_NIC_READY, 50, 200);
    if (rc != IWM_OK)
        return rc;
    wr(a, CSR_MBOX_SET_REG, CSR_MBOX_OS_ALIVE);
    v = rd(a, CSR_GP_CNTRL) | CSR_GP_INIT_DONE;
    wr(a, CSR_GP_CNTRL, v);
    rc = poll(a, CSR_GP_CNTRL, CSR_GP_CLOCK_READY,
              CSR_GP_CLOCK_READY, 250, 100);
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
