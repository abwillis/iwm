/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef IWM_OS2_H
#define IWM_OS2_H

#include "iwm_genmac.h"

struct iwm_os2_ioctl {
    uint8_t category;
    uint8_t function;
    uint32_t parameters[4];
    void *data;
    uint32_t data_length;
    uint32_t data_actual;
};

int iwm_os2_ioctl_dispatch(struct iwm_adapter *adapter,
                           struct iwm_os2_ioctl *request);

/* Implemented on the target from verified miniddk declarations; see
 * iwm_os2_adapter.h. MultiMac/MultiMac1 are reference-only, not build inputs. */
int iwm_os2_attach(struct iwm_adapter *adapter);
void iwm_os2_detach(struct iwm_adapter *adapter);
void iwm_os2_isr(struct iwm_adapter *adapter);
void iwm_os2_bottom_half(struct iwm_adapter *adapter);

#endif
