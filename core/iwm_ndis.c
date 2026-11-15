/* SPDX-License-Identifier: GPL-2.0-only */
#include "iwm_ndis.h"

int iwm_ndis_start(struct iwm_ndis_context *n)
{
    if (!n || !n->adapter || !n->callbacks || !n->callbacks->register_mac)
        return IWM_EINVAL;
    return n->callbacks->register_mac(n->callback_context, n->adapter);
}

int iwm_ndis_transmit(struct iwm_ndis_context *n,
                      const void *frame, uint16_t length, void *cookie)
{
    (void)frame;
    (void)length;
    if (!n || !n->adapter || !cookie)
        return IWM_EINVAL;
    if (n->adapter->state != IWM_ADAPTER_ASSOCIATED)
        return IWM_ESTATE;
    /* Milestone hook: map an NDIS packet into an IWM TX command and TFD. */
    return IWM_ENOTSUP;
}

void iwm_ndis_receive(struct iwm_ndis_context *n,
                      const void *frame, uint16_t length)
{
    if (n && n->callbacks && n->callbacks->indicate_receive)
        n->callbacks->indicate_receive(n->callback_context, frame, length);
}

void iwm_ndis_link_changed(struct iwm_ndis_context *n, int connected)
{
    if (!n || !n->adapter)
        return;
    n->adapter->connected = connected ? 1 : 0;
    n->adapter->state = connected ? IWM_ADAPTER_ASSOCIATED : IWM_ADAPTER_READY;
    ++n->adapter->status_generation;
    if (n->callbacks && n->callbacks->status_change)
        n->callbacks->status_change(n->callback_context,
                                    n->adapter->status_generation);
}

