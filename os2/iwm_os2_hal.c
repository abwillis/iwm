/* SPDX-License-Identifier: GPL-2.0-only */
#include "iwm_os2.h"

/*
 * This file is the boundary where OS/2 semantics replace BSD bus_dma and Linux
 * DMA APIs. The target implementation must use miniddk/Dev16lib services:
 *
 *  - PCI enumeration and BAR/IRQ discovery from the MultiMac PCI helpers.
 *  - physical contiguous allocations for descriptors and firmware bounce DMA;
 *  - GDT selectors or flat mappings for 16-bit access to those allocations;
 *  - SetIRQ/EOI interrupt registration and acknowledgement;
 *  - SMP-safe locking with interrupts disabled only for short register/ring
 *    critical sections.
 *
 * Never pass a ring-3 pointer to hardware and never use a general C heap block
 * as a DMA descriptor. The concrete wrappers belong here so the hardware core
 * remains testable on a host compiler.
 */

int iwm_os2_attach(struct iwm_adapter *adapter)
{
    if (!adapter || !adapter->cfg)
        return IWM_EINVAL;
    return IWM_ENOTSUP;
}

void iwm_os2_detach(struct iwm_adapter *adapter)
{
    if (adapter)
        iwm_free_rings(adapter);
}

void iwm_os2_isr(struct iwm_adapter *adapter)
{
    if (!adapter)
        return;
    /* Milestone hook: read/ack CSR/FH cause registers, mask and defer work. */
    ++adapter->status_generation;
}

void iwm_os2_bottom_half(struct iwm_adapter *adapter)
{
    if (!adapter)
        return;
    /* Milestone hook: drain RX, complete TX and process firmware notices. */
}
