/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef IWM_RX_H
#define IWM_RX_H

#include "iwm_hw.h"

/* 9000-family multi-queue RX queue 0.  The hardware table size is fixed at
 * 512 entries; one entry remains unposted to distinguish full from empty. */
#define IWM_RX_QUEUE_SIZE       512U
#define IWM_RX_QUEUE_MASK       (IWM_RX_QUEUE_SIZE - 1U)
#define IWM_RX_POSTED_COUNT     (IWM_RX_QUEUE_SIZE - 1U)
#define IWM_RX_RBD_ALIGNMENT    4096U
#define IWM_RX_STATUS_ALIGNMENT 16U
#define IWM_RX_FRAME_SIZE_MASK  0x00003fffUL
#define IWM_RX_CMD_ALIVE        0x01U
#define IWM_RX_ALIVE_STATUS_OK  0xcafeU
/* Linux SEQ_RX_FRAME: set in unsolicited notifications, clear in responses. */
#define IWM_RX_SEQ_RX_FRAME     0x8000U
#define IWM_RX_RESP_COPY        64U

struct iwm_rx_status {
    volatile uint16_t closed_rb_num;
    volatile uint16_t closed_fr_num;
    volatile uint16_t finished_rb_num;
    volatile uint16_t finished_fr_num;
    volatile uint32_t spare;
} IWM_PACKED;

struct iwm_rx_alive_info {
    uint16_t status;
    uint16_t flags;
    uint32_t lmac_major;
    uint32_t lmac_minor;
    uint8_t valid;
};

/* Called for every packet that is not the awaited command response. */
typedef void (*iwm_rx_notif_fn)(void *ctx, uint8_t cmd, uint8_t group,
                                uint16_t seq, const uint8_t *payload,
                                uint32_t len);

struct iwm_rx_queue {
    struct iwm_adapter *adapter;
    struct iwm_dma_block free_desc; /* 512 little-endian 64-bit entries */
    struct iwm_dma_block used_desc; /* 512 little-endian 32-bit buffer IDs */
    struct iwm_dma_block status;
    struct iwm_dma_block buffers;   /* 512 contiguous 4 KiB buffers */
    uint16_t read;
    uint16_t posted;
    uint16_t write_actual;
    uint16_t last_closed;
    uint32_t packets;
    uint32_t malformed;
    uint8_t programmed;
    uint8_t alive_seen;
    uint8_t last_cmd;
    uint8_t last_group;
    uint32_t widx_readback;  /* RFH_Q0_FRBDCB_WIDX right after the trigger */
    uint32_t widx_direct;    /* after a direct PRPH write; ~0 if not needed */
    struct iwm_rx_alive_info alive;
    uint16_t last_seq;
    /* Host-command response capture (iwm_rx_wait_resp). */
    uint8_t resp_wait;
    uint8_t resp_seen;
    uint16_t resp_seq;
    uint8_t resp_cmd;
    uint8_t resp_group;
    uint16_t resp_len;      /* payload length after the 4-byte header */
    uint8_t resp[IWM_RX_RESP_COPY];
    uint32_t notifications; /* packets with SEQ_RX_FRAME set */
    uint8_t *resp_buf;      /* optional larger copy of the response payload */
    uint16_t resp_cap;
    iwm_rx_notif_fn notif_cb;
    void *notif_ctx;
    uint8_t draining;        /* iwm_rx_drain() nesting depth */
    uint32_t nested_drains;  /* drains started from inside a callback */
};

void iwm_rx_init(struct iwm_rx_queue *queue, struct iwm_adapter *adapter);
int iwm_rx_alloc(struct iwm_rx_queue *queue);
void iwm_rx_free(struct iwm_rx_queue *queue);
int iwm_rx_program(struct iwm_rx_queue *queue);
int iwm_rx_drain(struct iwm_rx_queue *queue);
int iwm_rx_wait_alive(struct iwm_rx_queue *queue, uint32_t timeout_us);
/* Drain until a response whose sequence equals seq arrives.  The first
 * IWM_RX_RESP_COPY payload bytes are kept in queue->resp. */
int iwm_rx_wait_resp(struct iwm_rx_queue *queue, uint16_t seq,
                     uint32_t timeout_us);

/* Failure diagnostics: out[0] RFH_RXF_DMA_CFG, [1] RFH_RXF_RXQ_ACTIVE,
 * [2] RFH_Q0_FRBDCB_WIDX, [3] RFH_Q0_FRBDCB_RIDX, [4] status block
 * closed_rb_num | finished_rb_num << 16.  A zero read-back of [0]/[1] means
 * the RFH never accepted the programming; RIDX == 0 with both set means the
 * RFH never fetched a free descriptor (nothing arrived from firmware). */
void iwm_rx_debug(struct iwm_rx_queue *queue, uint32_t out[5]);

/* Parse one receive buffer.  Exposed for deterministic host tests. */
int iwm_rx_process_buffer(struct iwm_rx_queue *queue,
                          const uint8_t *buffer, uint32_t capacity);

#endif
