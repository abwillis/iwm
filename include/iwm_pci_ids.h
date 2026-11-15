/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef IWM_PCI_IDS_H
#define IWM_PCI_IDS_H

#include "iwm_types.h"

#define IWM_PCI_VENDOR_INTEL 0x8086U

enum iwm_device_family {
    IWM_FAMILY_7000 = 7000,
    IWM_FAMILY_8000 = 8000,
    IWM_FAMILY_9000 = 9000
};

struct iwm_device_config {
    uint16_t device_id;
    uint16_t family;
    const char *name;
    const char *firmware_alias;
    uint8_t host_interrupt_mode;
    uint8_t mq_rx;
    uint8_t integrated;
    uint8_t nvm_hw_section;
    uint16_t xtal_latency;
    /* Bit flags; IWM_CFG_SHARED_CLK maps to Linux FW_PHY_CFG_SHARED_CLK. */
#define IWM_CFG_SHARED_CLK 0x01U
    uint8_t phy_flags;
};

const struct iwm_device_config *iwm_lookup_device(uint16_t vendor,
                                                  uint16_t device);
size_t iwm_device_count(void);
const struct iwm_device_config *iwm_device_at(size_t index);

#endif

