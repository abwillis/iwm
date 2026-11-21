/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef IWM_TX_H
#define IWM_TX_H

#include "iwm_hw.h"

/* 9560/9000-family legacy FH/SCD transport geometry.  Linux allocates 31
 * queue byte-count tables and 256 hardware TFDs per queue; this bounded step
 * instantiates only the DQA command queue (queue 0, FIFO 7). */
#define IWM_TX_NUM_QUEUES          31U
#define IWM_TX_CMD_QUEUE           0U
/* DQA auxiliary queue (Linux mvm->aux_queue = 1).  The host only owns its TFD
 * ring and producer pointer; firmware configures the SCD through
 * SCD_QUEUE_CFG (Linux iwl_mvm_enable_txq with a NULL transport cfg). */
#define IWM_TX_AUX_QUEUE           1U
/* Frame queues used by Linux on this card (capture: CBBC 0x19e0/0x19e4
 * published at every load, SCD_QUEUE_CFG queue 5 = station 0 TID 8 FIFO 3
 * before authentication, queue 4 = station 0 TID 0 FIFO 1 after
 * association).  Each has a 256-entry TFD ring and a 32-slot buffer window. */
#define IWM_TX_DATA_QUEUE          4U
#define IWM_TX_MGMT_QUEUE          5U
#define IWM_TX_FRAME_QUEUES        2U
#define IWM_TX_FRAME_WINDOW        32U
#define IWM_TX_FRAME_BUF_SIZE      2048U
/* Linux struct iwl_tx_cmd (pre-gen2) is 56 bytes; the 802.11 header follows
 * it in the command, padded to a multiple of 4 (offload_assist MH_PAD). */
#define IWM_TX_CMD_TX              0x1cU
#define IWM_TX_TXCMD_SIZE          56U
#define IWM_TX_TXCMD_LEN_OFF       0U
#define IWM_TX_TXCMD_OFFLOAD_OFF   2U
#define IWM_TX_TXCMD_STA_OFF       16U
#define IWM_TX_TXCMD_DRAM_LSB_OFF  44U
#define IWM_TX_TXCMD_DRAM_MSB_OFF  48U
#define IWM_TX_OFFLOAD_MH_SHIFT    8U
#define IWM_TX_OFFLOAD_MH_PAD      0x2000U
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
#define IWM_TX_CMD_BUFFER_SIZE     4096U
/* Linux IWL_FIRST_TB_SIZE: the first TB of a host command is copied into the
 * per-slot first-TB buffer; the remainder is mapped from the command buffer. */
#define IWM_TX_FIRST_TB_COPY       20U
/* Legacy (group 0) host-command header: cmd, group, le16 sequence.
 * Wide (group != 0) header adds le16 length, reserved, version (Linux
 * struct iwl_cmd_header_wide). */
#define IWM_TX_CMD_HDR_SIZE        4U
#define IWM_TX_CMD_HDR_WIDE_SIZE   8U
#define IWM_TX_CMD_MAX_PAYLOAD     (IWM_TX_CMD_BUFFER_SIZE - IWM_TX_CMD_HDR_WIDE_SIZE)

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

struct iwm_tx_fq {
    struct iwm_dma_block tfds;
    struct iwm_dma_block first_tb;
    struct iwm_dma_block bufs;
    uint16_t read;
    uint16_t write;
    uint8_t queue;
    uint32_t sent;
    uint32_t done;
    uint32_t stale;           /* responses outside the in-flight window */
};

struct iwm_tx_transport {
    struct iwm_adapter *adapter;
    struct iwm_dma_block keep_warm;
    struct iwm_dma_block scd_bc_table;
    struct iwm_dma_block cmd_tfds;
    struct iwm_dma_block cmd_first_tb;
    struct iwm_dma_block cmd_buffers;
    struct iwm_dma_block aux_tfds;  /* queue 1 TFD ring, no frames queued */
    struct iwm_tx_fq fq[IWM_TX_FRAME_QUEUES];  /* queues 4 and 5 */
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
/* Queue one host command on queue 0 and ring the doorbell (Linux
 * iwl_pcie_enqueue_hcmd, pre-22000 TFD).  Group 0 uses the 4-byte legacy
 * header, any other group the 8-byte wide header.  *seq receives the
 * sequence the firmware echoes in its response. */
int iwm_tx_send_cmd(struct iwm_tx_transport *tx, uint8_t cmd, uint8_t group,
                    const void *payload, uint16_t length, uint16_t *seq);
/* Linux iwl_trans_pcie_txq_enable(cfg == NULL): set a data queue's producer
 * pointer (HBUS_TARG_WRPTR = ptr | queue << 8) before SCD_QUEUE_CFG. */
int iwm_tx_queue_set_ptr(struct iwm_tx_transport *tx, unsigned queue,
                         uint16_t ptr);
/* Queue one 802.11 frame on frame queue 4 or 5 (Linux iwl_trans_pcie_tx,
 * pre-gen2): command header (TX_CMD 0x1c, sequence queue << 8 | index),
 * the 56-byte TX command given by the caller (length, offload_assist and
 * scratch pointer are filled here), the 802.11 header padded to 4, then the
 * body.  TB0 is the first 20 bytes, TB1 the rest; the scheduler byte-count
 * entry is (len + 8) in dwords | sta_id << 12.  hdr_len is the 802.11 header
 * size (24 management, 26 QoS data). */
int iwm_tx_send_frame(struct iwm_tx_transport *tx, unsigned queue,
                      const uint8_t *txcmd, const uint8_t *frame,
                      uint16_t length, uint16_t hdr_len, uint16_t *seq);
/* TX response for seq (queue << 8 | index): reclaim through that slot.  A
 * response for a slot that is not in flight is ignored and counted in
 * iwm_tx_fq.stale. */
void iwm_tx_frame_done(struct iwm_tx_transport *tx, uint16_t seq);
/* Software indices of a frame queue back to 0 (queue (re)enabled at 0). */
int iwm_tx_frame_queue_reset(struct iwm_tx_transport *tx, unsigned queue);
struct iwm_tx_fq *iwm_tx_fq_get(struct iwm_tx_transport *tx, unsigned queue);
/* Reclaim command-queue slots up to and including the one for seq. */
void iwm_tx_cmd_done(struct iwm_tx_transport *tx, uint16_t seq);

void iwm_tx_debug_snapshot(struct iwm_tx_transport *tx,
                           struct iwm_tx_debug *debug);

#endif
