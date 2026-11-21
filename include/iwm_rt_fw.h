/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef IWM_RT_FW_H
#define IWM_RT_FW_H
#include "iwm_hw.h"
#include "iwm_rx.h"
#include "iwm_tx.h"
#include "iwm_init_fw.h"
#include "iwm_scan.h"

/* Milestone C: runtime (REGULAR) firmware after its ALIVE.
 *
 * Paging (Linux fw/paging.c, fw/api/paging.h): the REGULAR image carries a
 * PAGING separator (0xAAAABBBB), then a CSS section, then the CPU2 paging
 * image.  The CSS goes in a 4 KiB block 0; the paging image is split into
 * 32 KiB blocks (8 pages each), the last holding the remaining pages.  The
 * page-frame numbers are sent with FW_PAGING_BLOCK_CMD (LONG group).
 *
 * Non-unified 9000 firmware then needs, in Linux iwl_mvm_up() order: valid
 * TX antennas, the PHY-DB results collected from INIT, and the PHY
 * configuration with the REGULAR image's default calibration triggers. */
#define IWM_CMD_FW_PAGING_BLOCK       0x4fU
#define IWM_CMD_PHY_DB                0x6cU
#define IWM_GROUP_LONG                0x01U

#define IWM_PAGING_PAGE_SHIFT         12U
#define IWM_PAGING_PAGES_PER_BLOCK    8U
#define IWM_PAGING_BLOCK_SIZE         32768UL
#define IWM_PAGING_BLOCK_SHIFT        15U
#define IWM_PAGING_MAX_BLOCKS         33U   /* CSS + 32 */
#define IWM_PAGING_CMD_SECURED        0x00000200UL
#define IWM_PAGING_CMD_ENABLED        0x00000100UL
#define IWM_PAGING_CMD_SIZE           (12U + 4U * IWM_PAGING_MAX_BLOCKS)

/* Linux enum iwl_phy_db_section_type. */
#define IWM_PHY_DB_CFG                1U
#define IWM_PHY_DB_CALIB_NCH          2U
#define IWM_PHY_DB_CALIB_CHG_PAPD     4U
#define IWM_PHY_DB_CALIB_CHG_TXP      5U

#define IWM_RT_CMD_TIMEOUT_US         500000UL
/* Two commands in a row without a response: the firmware or its command queue
 * is dead.  Further commands fail at once (IWM_EIO) instead of each waiting
 * IWM_RT_CMD_TIMEOUT_US, which stalled every XWLAN scan request by 0.5 s. */
#define IWM_RT_DEAD_AFTER             2U
#define IWM_RT_NOTIF_LOG              12U

/* Linux runtime command sequence replayed from the kprobe capture
 * (core/iwm_rt_seq.c).  id = group << 8 | opcode. */
#define IWM_RT_SEQ_NO_MAC             0xffffU
#define IWM_RT_SEQ_NONFATAL           0x01U  /* log the result, keep going */
#define IWM_RT_SEQ_AUX_PTR            0x02U  /* HBUS_TARG_WRPTR = aux queue first */
#define IWM_RT_SEQ_SCAN               0x04U  /* then wait for scan complete */
#define IWM_RT_PRE_CMDS               3U
#define IWM_RT_POST_CMDS              21U

struct iwm_rt_seq_cmd {
    uint16_t id;
    uint16_t len;
    const uint8_t *data;       /* NULL: all-zero payload; trailing zeros implied */
    uint16_t mac_off;          /* adapter MAC goes here, or IWM_RT_SEQ_NO_MAC */
    uint8_t flags;
};
extern const struct iwm_rt_seq_cmd iwm_rt_pre_cmds[IWM_RT_PRE_CMDS];
extern const struct iwm_rt_seq_cmd iwm_rt_post_cmds[IWM_RT_POST_CMDS];

#define IWM_RT_CMD_LOG                32U
#define IWM_RT_HIST                   24U
#define IWM_RT_MPDU_SAMPLES           2U
#define IWM_RT_MPDU_SAMPLE_LEN        64U
#define IWM_RT_SCAN_TIMEOUT_US        10000000UL
/* Linux SCAN_COMPLETE_UMAC (legacy ID 0x0f) and REPLY_RX_MPDU_CMD (0xc1). */
#define IWM_CMD_SCAN_COMPLETE_UMAC    0x0fU
#define IWM_CMD_RX_MPDU               0xc1U

struct iwm_rt_cmd_rec {
    uint16_t id;
    uint16_t len;
    int32_t rc;
    uint16_t resp_len;
    uint32_t resp0;            /* first two response payload dwords */
    uint32_t resp1;
};

struct iwm_rt_hist {
    uint16_t key;              /* group << 8 | cmd */
    uint16_t max_len;
    uint32_t count;
};

enum iwm_rt_stage {
    IWM_RT_IDLE = 0,
    IWM_RT_PAGING_PREP,
    IWM_RT_PAGING_CMD,
    IWM_RT_TX_ANT,
    IWM_RT_PHY_DB,
    IWM_RT_PHY_CFG,
    IWM_RT_DONE,
    IWM_RT_POST_SEQ,
    IWM_RT_SCAN_WAIT,
    IWM_RT_SCAN_DONE
};

/* Every received 802.11 frame with a valid hardware status, delivered from
 * the RX path for the association layer. */
typedef void (*iwm_rt_mgmt_fn)(void *ctx, const uint8_t *frame, uint32_t length);
/* Every notification (any command/group), before the built-in handling. */
typedef void (*iwm_rt_notif_fn)(void *ctx, uint8_t cmd, uint8_t group,
                                uint16_t seq, const uint8_t *payload,
                                uint32_t length);

struct iwm_rt_fw {
    iwm_rt_mgmt_fn mgmt_cb;
    void *mgmt_ctx;
    iwm_rt_notif_fn notif_hook;
    void *notif_hook_ctx;
    uint32_t mgmt_frames;
    struct iwm_adapter *adapter;
    struct iwm_rx_queue *rxq;
    struct iwm_tx_transport *tx;
    const struct iwm_init_fw *init;   /* PHY-DB entries from INIT */
    struct iwm_dma_block css;         /* paging block 0 */
    struct iwm_dma_block pages;       /* paging blocks 1..n, contiguous */
    uint16_t paging_sep;              /* section index, 0xffff when absent */
    uint16_t css_sec;
    uint16_t data_sec;
    uint32_t paging_size;             /* bytes copied into blocks 1..n */
    uint32_t paging_blocks;           /* n (data blocks, CSS excluded) */
    uint32_t pages_last;              /* pages in block n */
    uint8_t stage;
    int32_t rc;
    uint8_t fail_cmd;
    uint16_t fail_seq;
    uint16_t cmd_timeouts;            /* all command timeouts since runtime ALIVE */
    uint16_t consec_timeouts;         /* in a row; reset by any response */
    uint8_t dead;                     /* consec_timeouts reached IWM_RT_DEAD_AFTER */
    uint16_t phydb_sent;
    uint16_t phydb_skipped;           /* superseded PAPD/TXP channel groups */
    uint32_t phydb_bytes;
    uint32_t phy_cfg;
    uint32_t flow_trigger;
    uint32_t event_trigger;
    uint32_t notif_count;
    uint32_t notif_log[IWM_RT_NOTIF_LOG]; /* cmd | group << 8 | len << 16 */
    uint8_t scratch[64];
    /* Every replayed command, in order. */
    struct iwm_rt_cmd_rec cmd_log[IWM_RT_CMD_LOG];
    uint16_t cmd_count;
    /* Notification histogram while the post-config sequence and scan run. */
    struct iwm_rt_hist hist[IWM_RT_HIST];
    uint16_t hist_n;
    uint32_t hist_lost;
    uint8_t scan_done;
    uint16_t scan_complete_len;
    uint8_t scan_complete[16]; /* iwl_umac_scan_complete: uid, ..., status */
    uint32_t scan_wait_us;
    uint16_t mpdu_n;
    uint16_t mpdu_len[IWM_RT_MPDU_SAMPLES];
    uint8_t mpdu[IWM_RT_MPDU_SAMPLES][IWM_RT_MPDU_SAMPLE_LEN];
    /* RX MPDU frames seen while a scan runs (stage POST_SEQ or SCAN_WAIT):
     * parsed into the adapter scan cache, or counted as skipped (not a
     * beacon / probe response), bad (status or format) or lost (cache). */
    uint32_t scan_frames;
    uint32_t scan_parsed;
    uint32_t scan_skipped;
    uint32_t scan_bad;
    uint32_t scan_lost;
    uint32_t scans;            /* completed scans since runtime ALIVE */
    struct iwm_scan_result scan_tmp; /* static scratch, not ring-0 stack */
};

void iwm_rt_fw_init(struct iwm_rt_fw *rt, struct iwm_adapter *a,
                    struct iwm_rx_queue *rxq, struct iwm_tx_transport *tx,
                    const struct iwm_init_fw *init);
void iwm_rt_fw_free(struct iwm_rt_fw *rt);
void iwm_rt_fw_set_mgmt_cb(struct iwm_rt_fw *rt, iwm_rt_mgmt_fn cb, void *ctx);
void iwm_rt_fw_set_notif_hook(struct iwm_rt_fw *rt, iwm_rt_notif_fn fn,
                              void *ctx);
/* Send one host command (id = group << 8 | opcode) after runtime ALIVE and
 * wait for its response.  Up to resp_cap (at most 64) response payload bytes
 * are copied to resp; *resp_len receives the full payload length. */
int iwm_rt_fw_send(struct iwm_rt_fw *rt, uint16_t id, const uint8_t *payload,
                   uint16_t length, uint8_t *resp, uint16_t resp_cap,
                   uint16_t *resp_len);
/* Locate the paging sections of the REGULAR image, allocate and fill the
 * DMA blocks.  Returns IWM_OK with paging_blocks == 0 when the image has no
 * paging.  Call before or after the runtime load. */
int iwm_rt_fw_prepare_paging(struct iwm_rt_fw *rt);
/* After a validated runtime ALIVE with the scheduler started: paging
 * command, TX antennas, PHY-DB replay, PHY configuration. */
int iwm_rt_fw_run(struct iwm_rt_fw *rt);
/* After iwm_rt_fw_run(): the rest of the Linux runtime sequence (BT, SOC,
 * DQA, aux queue/station, power, MCC, scan config, MAC context), then one
 * UMAC scan request; waits for SCAN_COMPLETE_UMAC.  Notifications are
 * counted per ID and the first RX MPDU frames are sampled for the scan-result
 * parser.  Requires a valid adapter MAC address. */
int iwm_rt_fw_scan_probe(struct iwm_rt_fw *rt);
/* One more UMAC scan with the configuration iwm_rt_fw_scan_probe() left in
 * place (stage SCAN_DONE, or SCAN_WAIT after a timeout).  Results go into the
 * adapter scan cache bound with iwm_genmac_bind_scan_cache(). */
int iwm_rt_fw_scan(struct iwm_rt_fw *rt);
/* Build the FW_PAGING_BLOCK_CMD payload (IWM_PAGING_CMD_SIZE bytes);
 * exposed for host tests. */
void iwm_rt_fw_paging_cmd(const struct iwm_rt_fw *rt, uint8_t *out);
#endif
