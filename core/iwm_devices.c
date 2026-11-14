/* SPDX-License-Identifier: GPL-2.0-only */
#include "iwm_pci_ids.h"

/* Device IDs and family mappings follow the FreeBSD iwm reference snapshot. */
static const struct iwm_device_config devices[] = {
    {0x08b3, IWM_FAMILY_7000, "Intel Dual Band Wireless-AC 3160", "iwm3160fw", 1, 0, 0, 0, 0},
    {0x08b4, IWM_FAMILY_7000, "Intel Dual Band Wireless-AC 3160", "iwm3160fw", 1, 0, 0, 0, 0},
    {0x3165, IWM_FAMILY_7000, "Intel Dual Band Wireless-AC 3165", "iwm7265Dfw", 0, 0, 0, 0, 0},
    {0x3166, IWM_FAMILY_7000, "Intel Dual Band Wireless-AC 3165", "iwm7265Dfw", 0, 0, 0, 0, 0},
    {0x24fb, IWM_FAMILY_7000, "Intel Dual Band Wireless-AC 3168", "iwm3168fw", 0, 0, 0, 0, 0},
    {0x08b1, IWM_FAMILY_7000, "Intel Dual Band Wireless-AC 7260", "iwm7260fw", 1, 0, 0, 0, 0},
    {0x08b2, IWM_FAMILY_7000, "Intel Dual Band Wireless-AC 7260", "iwm7260fw", 1, 0, 0, 0, 0},
    {0x095a, IWM_FAMILY_7000, "Intel Dual Band Wireless-AC 7265", "iwm7265fw", 0, 0, 0, 0, 0},
    {0x095b, IWM_FAMILY_7000, "Intel Dual Band Wireless-AC 7265", "iwm7265fw", 0, 0, 0, 0, 0},
    {0x24f3, IWM_FAMILY_8000, "Intel Dual Band Wireless-AC 8260", "iwm8000Cfw", 0, 0, 0, 10, 0},
    {0x24f4, IWM_FAMILY_8000, "Intel Dual Band Wireless-AC 8260", "iwm8000Cfw", 0, 0, 0, 10, 0},
    {0x24fd, IWM_FAMILY_8000, "Intel Dual Band Wireless-AC 8265", "iwm8265fw", 0, 0, 0, 10, 0},
    {0x9df0, IWM_FAMILY_9000, "Intel Dual Band Wireless-AC 9560", "iwm9000fw", 0, 1, 1, 10, 650},
    {0xa370, IWM_FAMILY_9000, "Intel Dual Band Wireless-AC 9560", "iwm9000fw", 0, 1, 1, 10, 650},
    {0x31dc, IWM_FAMILY_9000, "Intel Dual Band Wireless-AC 9560", "iwm9000fw", 0, 1, 1, 10, 650},
    {0x2526, IWM_FAMILY_9000, "Intel Dual Band Wireless-AC 9260", "iwm9260fw", 0, 1, 0, 10, 0}
};

const struct iwm_device_config *iwm_lookup_device(uint16_t vendor,
                                                  uint16_t device)
{
    size_t i;
    if (vendor != IWM_PCI_VENDOR_INTEL)
        return NULL;
    for (i = 0; i < IWM_ARRAY_SIZE(devices); ++i)
        if (devices[i].device_id == device)
            return &devices[i];
    return NULL;
}

size_t iwm_device_count(void) { return IWM_ARRAY_SIZE(devices); }

const struct iwm_device_config *iwm_device_at(size_t index)
{
    return index < IWM_ARRAY_SIZE(devices) ? &devices[index] : NULL;
}
