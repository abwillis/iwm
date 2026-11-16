/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef IWM_FW_LOADER_H
#define IWM_FW_LOADER_H

#include "iwm_hw.h"
#include "iwm_transport.h"

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
    struct iwm_dma_block bounce;
    volatile uint32_t irq_cause;
    uint32_t loaded_bytes;
    uint32_t last_dst;
    uint32_t last_csr_int;
    uint32_t last_fh_int;
    uint32_t last_load_status;
    uint16_t loaded_sections;
    uint16_t stage;
    int32_t last_error;
    uint8_t alive_interrupt;
};

void iwm_fw_loader_init(struct iwm_fw_loader *loader,
                        struct iwm_adapter *adapter);
int iwm_fw_loader_alloc(struct iwm_fw_loader *loader);
void iwm_fw_loader_free(struct iwm_fw_loader *loader);

/* Called by the hard ISR after it has acknowledged CSR_INT. */
void iwm_fw_loader_note_irq(struct iwm_fw_loader *loader, uint32_t cause);

/* Load either IWM_FW_SEC_INIT or IWM_FW_SEC_RUNTIME through the FH service
 * channel.  Success requires the early firmware ALIVE interrupt.  This still
 * does not promote firmware to RUNNING: a real ALIVE RX notification must be
 * parsed by the notification queue first. */
int iwm_fw_loader_start(struct iwm_fw_loader *loader, uint8_t image_flag);

/* Exposed for deterministic tests and diagnostics.  Returns 1 for CPU1, 2 for
 * CPU2, or 0 for separators/paging/not part of the selected image. */
int iwm_fw_section_cpu(const struct iwm_fw_image *image, uint16_t index,
                       uint8_t image_flag);

#endif


