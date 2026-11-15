/* SPDX-License-Identifier: GPL-2.0-only */
/* Ring 0: Dev32lib.h (forced in with -fi) declares memcpy/memset and the
 * miniddk string.h conflicts with it. */
#if !defined(IWM_RING0)
#include <string.h>
#endif
#include "iwm_hw.h"

int iwm_plan_rings(const struct iwm_adapter *a, struct iwm_ring_plan *plan)
{
    if (!a || !a->cfg || !plan)
        return IWM_EINVAL;
    memset(plan, 0, sizeof(*plan));
    plan->tx_count = IWM_TX_RING_COUNT;
    plan->rx_count = a->cfg->mq_rx ? IWM_RX_MQ_RING_COUNT :
                                     IWM_RX_LEGACY_RING_COUNT;
    /* IWM TFDs are 128 bytes; RX free descriptors are 64-bit addresses. */
    plan->tx_desc_size = 128;
    plan->rx_desc_size = 8;
    plan->tx_desc_bytes = (uint32_t)plan->tx_count * plan->tx_desc_size;
    plan->rx_desc_bytes = (uint32_t)plan->rx_count * plan->rx_desc_size;
    return IWM_OK;
}

int iwm_allocate_rings(struct iwm_adapter *a)
{
    int rc;
    if (!a || !a->ops || !a->ops->dma_alloc)
        return IWM_EINVAL;
    rc = a->ops->dma_alloc(a->os_context, a->ring_plan.tx_desc_bytes,
                           256, &a->tx_desc);
    if (rc != IWM_OK)
        return rc;
    rc = a->ops->dma_alloc(a->os_context, a->ring_plan.rx_desc_bytes,
                           256, &a->rx_desc);
    if (rc != IWM_OK)
        goto fail_rx;
    rc = a->ops->dma_alloc(a->os_context, IWM_ICT_SIZE,
                           IWM_ICT_SIZE, &a->ict);
    if (rc != IWM_OK)
        goto fail_ict;
    memset(a->tx_desc.vaddr, 0, a->tx_desc.size);
    memset(a->rx_desc.vaddr, 0, a->rx_desc.size);
    memset(a->ict.vaddr, 0, a->ict.size);
    return IWM_OK;
fail_ict:
    a->ops->dma_free(a->os_context, &a->rx_desc);
fail_rx:
    a->ops->dma_free(a->os_context, &a->tx_desc);
    return rc;
}

void iwm_free_rings(struct iwm_adapter *a)
{
    if (!a || !a->ops || !a->ops->dma_free)
        return;
    if (a->ict.vaddr) a->ops->dma_free(a->os_context, &a->ict);
    if (a->rx_desc.vaddr) a->ops->dma_free(a->os_context, &a->rx_desc);
    if (a->tx_desc.vaddr) a->ops->dma_free(a->os_context, &a->tx_desc);
    memset(&a->ict, 0, sizeof(a->ict));
    memset(&a->rx_desc, 0, sizeof(a->rx_desc));
    memset(&a->tx_desc, 0, sizeof(a->tx_desc));
}

