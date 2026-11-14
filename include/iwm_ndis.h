/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef IWM_NDIS_H
#define IWM_NDIS_H

#include "iwm_hw.h"

/* OS/2 NDIS glue is deliberately isolated from the hardware core. */
struct iwm_ndis_callbacks {
    int (*register_mac)(void *context, struct iwm_adapter *adapter);
    int (*indicate_receive)(void *context, const void *frame, uint16_t length);
    void (*transmit_complete)(void *context, void *packet, int status);
    void (*status_change)(void *context, uint32_t generation);
};

struct iwm_ndis_context {
    const struct iwm_ndis_callbacks *callbacks;
    void *callback_context;
    struct iwm_adapter *adapter;
};

int iwm_ndis_start(struct iwm_ndis_context *ndis);
int iwm_ndis_transmit(struct iwm_ndis_context *ndis,
                      const void *frame, uint16_t length, void *cookie);
void iwm_ndis_receive(struct iwm_ndis_context *ndis,
                      const void *frame, uint16_t length);
void iwm_ndis_link_changed(struct iwm_ndis_context *ndis, int connected);

#endif
