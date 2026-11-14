/* SPDX-License-Identifier: GPL-2.0-only */
#include "iwm_bar.h"

int iwm_bar_decode(uint32_t lo, uint32_t hi, uint32_t *phys)
{
    uint32_t base;

    if (!phys)
        return IWM_EINVAL;
    if (lo & IWM_BAR_IO)
        return IWM_EINVAL;
    base = lo & ~IWM_BAR_FLAGS_MASK;
    switch (lo & IWM_BAR_TYPE_MASK) {
    case IWM_BAR_TYPE_32:
        break;
    case IWM_BAR_TYPE_64:
        if (hi != 0)
            return IWM_ERANGE;
        break;
    default:
        return IWM_EINVAL; /* below-1M and reserved types */
    }
    if (base == 0)
        return IWM_EINVAL;
    *phys = base;
    return IWM_OK;
}

int iwm_bar_size(uint32_t sized_lo, uint32_t sized_hi, int is64,
                 uint32_t *size)
{
    uint32_t mask;

    if (!size)
        return IWM_EINVAL;
    mask = sized_lo & ~IWM_BAR_FLAGS_MASK;
    if (mask == 0)
        return IWM_EINVAL;
    if (is64 && sized_hi != 0xFFFFFFFFUL)
        return IWM_ERANGE;
    *size = (uint32_t)(~mask + 1UL);
    return IWM_OK;
}
