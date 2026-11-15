/* SPDX-License-Identifier: GPL-2.0-only */
#include "assert.h"
#include "stdio.h"
#include "iwm_transport.h"

int main(void)
{
    struct iwm_irq_result r;
    uint16_t v;

    /* Shared INTx: zero cause is not ours. */
    assert(iwm_irq_decode(0, &r) == IWM_OK && !r.claimed && r.ack == 0);
    /* All-ones read: device gone. */
    assert(iwm_irq_decode(0xFFFFFFFFUL, &r) == IWM_ENODEV && r.gone);
    /* ALIVE and RX are deferred and acknowledged. */
    assert(iwm_irq_decode(IWM_CSR_INT_ALIVE | IWM_CSR_INT_FH_RX, &r) == IWM_OK);
    assert(r.claimed && !r.fatal);
    assert(r.ack == (IWM_CSR_INT_ALIVE | IWM_CSR_INT_FH_RX));
    assert(r.deferred == (IWM_CSR_INT_ALIVE | IWM_CSR_INT_FH_RX));
    /* Firmware or DMA error is fatal. */
    assert(iwm_irq_decode(IWM_CSR_INT_SW_ERR, &r) == IWM_OK && r.fatal);
    assert(iwm_irq_decode(IWM_CSR_INT_HW_ERR, &r) == IWM_OK && r.fatal);
    /* Unknown bits are still acknowledged but reported. */
    assert(iwm_irq_decode(0x00000100UL, &r) == IWM_ENOTSUP && r.ack == 0x100);

    /* 32-bit DMA windows. */
    assert(iwm_dma32_check(0x00100000UL, 4096, 4096) == IWM_OK);
    assert(iwm_dma32_check(0x00100100UL, 4096, 4096) == IWM_EINVAL);
    assert(iwm_dma32_check(0xFFFFF000UL, 4096, 4096) == IWM_OK);
    assert(iwm_dma32_check(0xFFFFF000UL, 8192, 4096) == IWM_ERANGE);
    assert(iwm_dma32_check(0x1000, 0, 256) == IWM_EINVAL);
    assert(iwm_dma32_check(0x1000, 16, 3) == IWM_EINVAL);

    /* Ring arithmetic. */
    assert(iwm_ring_next(255, 256, &v) == IWM_OK && v == 0);
    assert(iwm_ring_next(5, 256, &v) == IWM_OK && v == 6);
    assert(iwm_ring_next(256, 256, &v) == IWM_EINVAL);
    assert(iwm_ring_next(0, 100, &v) == IWM_EINVAL);
    assert(iwm_ring_space(0, 0, 256, &v) == IWM_OK && v == 255);
    assert(iwm_ring_space(0, 255, 256, &v) == IWM_OK && v == 0);
    assert(iwm_ring_space(10, 5, 256, &v) == IWM_OK && v == 4);

    puts("transport tests passed");
    return 0;
}


