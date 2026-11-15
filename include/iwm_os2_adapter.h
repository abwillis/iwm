/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef IWM_OS2_ADAPTER_H
#define IWM_OS2_ADAPTER_H

#include "iwm_types.h"

struct iwm_adapter;

/*
 * Target bridge. Every callback is implemented on OS/2 from verified miniddk
 * declarations (miniddk before ddk). No OS/2, MultiMac or DDK header is
 * included here, so the attach/rollback ordering is host-testable.
 *
 *   pci_attach     find the PCI function, enable memory + bus master
 *   map_bar        map BAR0 for CSR MMIO
 *   bind_fw_store  fixed physical firmware store, GDT selector windows,
 *                  then iwm_adapter_set_fw_store()
 *   install_irq    DevHelp SetIRQ; ISR acknowledges, context hook defers
 * The matching release callbacks undo each step and must be idempotent.
 */
struct iwm_os2_target_api {
    int (*pci_attach)(void *context, struct iwm_adapter *adapter);
    void (*pci_detach)(void *context, struct iwm_adapter *adapter);
    int (*map_bar)(void *context, struct iwm_adapter *adapter);
    void (*unmap_bar)(void *context, struct iwm_adapter *adapter);
    int (*bind_fw_store)(void *context, struct iwm_adapter *adapter);
    void (*release_fw_store)(void *context, struct iwm_adapter *adapter);
    int (*install_irq)(void *context, struct iwm_adapter *adapter);
    void (*remove_irq)(void *context, struct iwm_adapter *adapter);
};

struct iwm_os2_target {
    const struct iwm_os2_target_api *api;
    void *context;
    uint8_t stage;  /* number of attach steps that succeeded */
};

int iwm_os2_target_attach(struct iwm_os2_target *target,
                          struct iwm_adapter *adapter);
void iwm_os2_target_detach(struct iwm_os2_target *target,
                           struct iwm_adapter *adapter);

#endif

