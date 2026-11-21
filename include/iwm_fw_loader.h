/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef IWM_FW_LOADER_H
#define IWM_FW_LOADER_H

#include "iwm_hw.h"
#include "iwm_transport.h"

struct iwm_rx_queue;
struct iwm_tx_transport;

/* The service-channel transfer buffer is deliberately small.  It remains
 * below iwm_blob_read()'s 16-bit length limit and below FH's 128 KiB limit. */
#define IWM_FW_DMA_CHUNK_SIZE       16384U
#define IWM_FW_DMA_ALIGNMENT       16U
#define IWM_FW_LOAD_TIMEOUT_US     5000000UL
#define IWM_FW_ALIVE_TIMEOUT_US    1000000UL

/* Progress values are stable diagnostics, not firmware protocol states. */
enum iwm_fw_load_stage {
    IWM_FWLS_IDLE = 0,
    IWM_FWLS_ALLOCATED,
    IWM_FWLS_PREPARED,
    IWM_FWLS_CPU1,
    IWM_FWLS_CPU2,
    IWM_FWLS_RELEASED,
    IWM_FWLS_ALIVE_IRQ,
    IWM_FWLS_FAILED
};

struct iwm_fw_loader {
    struct iwm_adapter *adapter;
    struct iwm_rx_queue *rxq;
    struct iwm_tx_transport *tx;
    struct iwm_dma_block bounce;
    volatile uint32_t irq_cause;
    uint32_t loaded_bytes;
    uint32_t last_dst;
    uint32_t last_csr_int;
    uint32_t last_fh_int;
    uint32_t last_load_status;
    uint32_t last_gp_cntrl;
    uint32_t irq_seen;     /* OR of every CSR_INT cause the ISR delivered */
    uint16_t irq_calls;    /* ISR deliveries during this load */
    uint16_t alive_at;     /* delivery number of the first ALIVE cause, 0 none */
    uint16_t err_at;       /* delivery number of the first SW/HW_ERR, 0 none */
    uint32_t early_sb[2][4]; /* SB1 SB2 UMAC-PC LMAC-PC at +0 and +10 ms */
    uint32_t last_hw_if;   /* CSR_HW_IF_CONFIG_REG at failure */
    uint32_t last_sb[4];   /* SB CPU1/CPU2 status, UMAC PC, LMAC PC */
/* LMAC: valid error_id trm0 trm1 blink2 ilink1 ilink2 data1 data2 data3
 * bcon tsf_lo tsf_hi gp1 gp2 fw_rev_type major minor hw_ver brd_ver
 * log_pc frame_ptr stack_ptr hcmd (FreeBSD struct iwm_error_event_table).
 * UMAC uses the first 15 (struct iwm_umac_error_event_table). */
#define IWM_FW_ERRTAB_DWORDS 24U
    uint32_t last_lmac_err[IWM_FW_ERRTAB_DWORDS]; /* valid, error_id, ... */
    uint32_t last_umac_err[IWM_FW_ERRTAB_DWORDS];
    uint32_t last_lmac_err_addr;
    uint32_t last_umac_err_addr;
    uint32_t last_rxbuf[8];  /* first 8 dwords of RX buffer 0 at failure */
    uint32_t last_used[4];   /* used-descriptor ring entries 0..3 */
    uint32_t last_rfh[5]; /* DMA_CFG, RXQ_ACTIVE, WIDX, RIDX, closed|finished<<16 */
    uint32_t last_tx[12]; /* keep-warm, CBBC, FH0/7, chicken, SCD state */
    uint8_t last_tx_state[3]; /* allocated, prepared, scheduler started */
    uint16_t loaded_sections;
    uint16_t stage;
    int32_t last_error;
    uint8_t alive_interrupt;
    uint8_t alive_notification;
};

void iwm_fw_loader_init(struct iwm_fw_loader *loader,
                        struct iwm_adapter *adapter);
void iwm_fw_loader_bind_rx(struct iwm_fw_loader *loader,
                           struct iwm_rx_queue *queue);
void iwm_fw_loader_bind_tx(struct iwm_fw_loader *loader,
                           struct iwm_tx_transport *transport);
int iwm_fw_loader_alloc(struct iwm_fw_loader *loader);
void iwm_fw_loader_free(struct iwm_fw_loader *loader);

/* Called by the hard ISR after it has acknowledged CSR_INT. */
void iwm_fw_loader_note_irq(struct iwm_fw_loader *loader, uint32_t cause);

/* Load either IWM_FW_SEC_INIT or IWM_FW_SEC_RUNTIME through the FH service
 * channel. Success requires a validated ALIVE packet from the RFH receive
 * queue; the CSR ALIVE bit remains diagnostic only. INIT ALIVE does not by
 * itself promote the adapter to RUNNING. */
int iwm_fw_loader_start(struct iwm_fw_loader *loader, uint8_t image_flag);

/* Exposed for deterministic tests and diagnostics.  Returns 1 for CPU1, 2 for
 * CPU2, or 0 for separators/paging/not part of the selected image. */
int iwm_fw_section_cpu(const struct iwm_fw_image *image, uint16_t index,
                       uint8_t image_flag);

#endif


