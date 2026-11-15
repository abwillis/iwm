/* SPDX-License-Identifier: GPL-2.0-only */
#include "iwm_transport.h"

int iwm_irq_decode(uint32_t cause, struct iwm_irq_result *r)
{
    if (!r)
        return IWM_EINVAL;
    r->ack = 0;
    r->deferred = 0;
    r->claimed = 0;
    r->fatal = 0;
    r->gone = 0;
    if (cause == 0xFFFFFFFFUL) {
        r->gone = 1;
        r->claimed = 1;
        return IWM_ENODEV;
    }
    if (cause == 0)
        return IWM_OK;
    r->claimed = 1;
    r->ack = cause;
    r->deferred = cause & (IWM_CSR_INT_FH_RX | IWM_CSR_INT_SW_RX |
                           IWM_CSR_INT_RX_PERIODIC | IWM_CSR_INT_FH_TX |
                           IWM_CSR_INT_ALIVE | IWM_CSR_INT_RF_KILL |
                           IWM_CSR_INT_CT_KILL | IWM_CSR_INT_WAKEUP |
                           IWM_CSR_INT_RESET_DONE | IWM_CSR_INT_SCD);
    if (cause & (IWM_CSR_INT_HW_ERR | IWM_CSR_INT_SW_ERR))
        r->fatal = 1;
    return (cause & ~IWM_CSR_INT_KNOWN) ? IWM_ENOTSUP : IWM_OK;
}

int iwm_dma32_check(uint32_t paddr, uint32_t size, uint32_t alignment)
{
    if (size == 0 || alignment == 0 || (alignment & (alignment - 1U)))
        return IWM_EINVAL;
    if (paddr & (alignment - 1U))
        return IWM_EINVAL;
    if (paddr > 0xFFFFFFFFUL - (size - 1U))
        return IWM_ERANGE;
    return IWM_OK;
}

int iwm_ring_next(uint16_t index, uint16_t count, uint16_t *next)
{
    if (!next || count < 2 || (count & (count - 1U)) || index >= count)
        return IWM_EINVAL;
    *next = (uint16_t)((index + 1U) & (count - 1U));
    return IWM_OK;
}

int iwm_ring_space(uint16_t read, uint16_t write, uint16_t count,
                   uint16_t *space)
{
    if (!space || count < 2 || (count & (count - 1U)) ||
        read >= count || write >= count)
        return IWM_EINVAL;
    /* One slot stays empty so full and empty are distinguishable. */
    *space = (uint16_t)((read - write - 1U) & (count - 1U));
    return IWM_OK;
}


