/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef IWM_TRANSPORT_H
#define IWM_TRANSPORT_H

#include "iwm_types.h"

/*
 * Portable transport helpers. CSR_INT cause bits follow current Linux
 * iwlwifi iwl-csr.h; the host acknowledges a cause by writing 1 to it.
 */
#define IWM_CSR_INT_FH_RX        0x80000000UL
#define IWM_CSR_INT_HW_ERR       0x20000000UL
#define IWM_CSR_INT_RX_PERIODIC  0x10000000UL
#define IWM_CSR_INT_FH_TX        0x08000000UL
#define IWM_CSR_INT_SCD          0x04000000UL
#define IWM_CSR_INT_SW_ERR       0x02000000UL
#define IWM_CSR_INT_RF_KILL      0x00000080UL
#define IWM_CSR_INT_CT_KILL      0x00000040UL
#define IWM_CSR_INT_SW_RX        0x00000008UL
#define IWM_CSR_INT_RESET_DONE   0x00000004UL
#define IWM_CSR_INT_WAKEUP       0x00000002UL
#define IWM_CSR_INT_ALIVE        0x00000001UL

/* Linux CSR_INI_SET_MASK. */
#define IWM_CSR_INI_SET_MASK (IWM_CSR_INT_FH_RX | IWM_CSR_INT_HW_ERR | \
                              IWM_CSR_INT_FH_TX | IWM_CSR_INT_SW_ERR | \
                              IWM_CSR_INT_RF_KILL | IWM_CSR_INT_SW_RX | \
                              IWM_CSR_INT_WAKEUP | IWM_CSR_INT_RESET_DONE | \
                              IWM_CSR_INT_ALIVE | IWM_CSR_INT_RX_PERIODIC)

#define IWM_CSR_INT_KNOWN (IWM_CSR_INI_SET_MASK | IWM_CSR_INT_SCD | \
                           IWM_CSR_INT_CT_KILL)

struct iwm_irq_result {
    uint32_t ack;       /* bits to write back to CSR_INT */
    uint32_t deferred;  /* causes for the context-hook worker */
    uint8_t claimed;    /* 0: not ours (shared INTx) */
    uint8_t fatal;      /* HW_ERR or SW_ERR: stop DMA and reset */
    uint8_t gone;       /* all-ones read: device removed or powered off */
};

/* Classify a CSR_INT snapshot. Safe for hard-interrupt context. */
int iwm_irq_decode(uint32_t cause, struct iwm_irq_result *result);

/* Validate a DMA block for 32-bit FH descriptors. */
int iwm_dma32_check(uint32_t paddr, uint32_t size, uint32_t alignment);

/* Circular ring index arithmetic; count must be a power of two. */
int iwm_ring_next(uint16_t index, uint16_t count, uint16_t *next);
int iwm_ring_space(uint16_t read, uint16_t write, uint16_t count,
                   uint16_t *space);

#endif
