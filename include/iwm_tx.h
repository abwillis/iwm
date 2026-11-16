/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef IWM_TX_H
#define IWM_TX_H

#include "iwm_hw.h"

/* 9560/9000-family legacy FH/SCD transport geometry.  Linux allocates 31
 * queue byte-count tables and 256 hardware TFDs per queue; this bounded step
 * instantiates only the DQA command queue (queue 0, FIFO 7). */
#define IWM_TX_NUM_QUEUES          31U
#define IWM_TX_CMD_QUEUE           0U
#define IWM_TX_CMD_FIFO            7U
#define IWM_TX_HW_QUEUE_SIZE       256U
#define IWM_TX_CMD_WINDOW          32U
#define IWM_TX_TFD_SIZE            128U
#define IWM_TX_TFD_ALIGNMENT       256U
#define IWM_TX_KEEP_WARM_SIZE      4096U
#define IWM_TX_KEEP_WARM_ALIGNMENT 4096U
#define IWM_TX_BC_ENTRIES          320U
#define IWM_TX_BC_ENTRY_SIZE       2U
#define IWM_TX_BC_ALIGNMENT        1024U
#define IWM_TX_FIRST_TB_SIZE       64U
#define IWM_TX_CMD_BUFFER_SIZE     512U

struct iwm_tx_debug {
    uint32_t keep_warm;
    uint32_t cmd_cbbc;
    uint32_t fh_config_0;
    uint32_t fh_config_7;
    uint32_t fh_chicken;
    uint32_t scd_sram_base;
    uint32_t scd_dram_base;
    uint32_t scd_txfact;
    uint32_t scd_cmd_rdptr;
    uint32_t scd_cmd_wrptr;
    uint32_t scd_cmd_status;
    uint32_t hbus_wrptr;
    uint8_t allocated;
    uint8_t prepared;
    uint8_t scheduler_started;
};

struct iwm_tx_transport {
    struct iwm_adapter *adapter;
    struct iwm_dma_block keep_warm;
    struct iwm_dma_block scd_bc_table;
    struct iwm_dma_block cmd_tfds;
    struct iwm_dma_block cmd_first_tb;
    struct iwm_dma_block cmd_buffers;
    uint32_t scd_base;
    uint16_t read;
    uint16_t write;
    uint8_t allocated;
    uint8_t prepared;
    uint8_t scheduler_started;
};

void iwm_tx_init(struct iwm_tx_transport *tx, struct iwm_adapter *adapter);
int iwm_tx_alloc(struct iwm_tx_transport *tx);
void iwm_tx_free(struct iwm_tx_transport *tx);

/* Linux ordering for pre-ALIVE transport setup: stop SCD FIFOs, publish the
 * keep-warm page and command TFD ring, but do not start the scheduler yet. */
int iwm_tx_prepare(struct iwm_tx_transport *tx);

/* Linux starts the SCD only after a valid firmware ALIVE notification. */
int iwm_tx_start_scheduler(struct iwm_tx_transport *tx);
void iwm_tx_stop(struct iwm_tx_transport *tx);

/* Doorbell helper for the later host-command patch.  No command is submitted
 * by this transport-only step; the bounded producer index is testable now. */
int iwm_tx_set_write_ptr(struct iwm_tx_transport *tx, uint16_t write_ptr);

void iwm_tx_debug_snapshot(struct iwm_tx_transport *tx,
                           struct iwm_tx_debug *debug);

#endif
