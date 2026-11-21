/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef IWM_INIT_FW_H
#define IWM_INIT_FW_H
#include "iwm_hw.h"
#include "iwm_rx.h"
#include "iwm_tx.h"

/* INIT firmware host-command sequence (Linux iwl_run_init_mvm_ucode() as
 * captured on this 9560): NVM_ACCESS reads of sections 0..12, valid TX
 * antennas, PHY configuration, PHY-DB / INIT-complete notifications, MCC
 * query.  Runtime firmware is the next milestone. */
#define IWM_CMD_INIT_COMPLETE_NOTIF   0x04U
#define IWM_CMD_PHY_CONFIGURATION     0x6aU
#define IWM_CMD_CALIB_RES_PHY_DB      0x6bU
#define IWM_CMD_NVM_ACCESS            0x88U
#define IWM_CMD_TX_ANT_CONFIGURATION  0x98U
#define IWM_CMD_MCC_UPDATE            0xc8U

#define IWM_NVM_NUM_SECTIONS          13U
#define IWM_NVM_CHUNK                 0x800U
#define IWM_NVM_POOL_SIZE             49152U
/* Linux INIT on this card: CFG 48 + NCH 4 + PAPD 12 + 8 x TXP 3080 bytes
 * plus 4-byte entry headers = 24748 bytes. */
#define IWM_PHYDB_POOL_SIZE           32768U
#define IWM_PHYDB_HDR_SIZE            4U
#define IWM_INIT_CMD_TIMEOUT_US       500000UL
#define IWM_INIT_CALIB_TIMEOUT_US     2000000UL
#define IWM_FW_PHY_CFG_SHARED_CLK     0x80000000UL

enum iwm_init_stage {
    IWM_INIT_IDLE = 0,
    IWM_INIT_NVM,
    IWM_INIT_TX_ANT,
    IWM_INIT_PHY_CFG,
    IWM_INIT_CALIB_WAIT,
    IWM_INIT_MCC,
    IWM_INIT_DONE,
    IWM_INIT_STOPPED
};

struct iwm_nvm_section {
    uint32_t offset;   /* into pool */
    uint16_t length;
    uint8_t present;
    uint8_t chunks;
    uint16_t status;   /* firmware status of the last chunk read */
};

struct iwm_init_fw {
    struct iwm_adapter *adapter;
    struct iwm_rx_queue *rxq;
    struct iwm_tx_transport *tx;
    struct iwm_dma_block pool;      /* NVM sections, then PHY-DB entries */
    struct iwm_nvm_section nvm[IWM_NVM_NUM_SECTIONS];
    uint32_t nvm_used;
    uint32_t phydb_used;            /* entries: le16 type, le16 len, data */
    uint16_t phydb_count;
    uint8_t init_complete;
    uint8_t stage;
    int32_t rc;
    uint8_t fail_cmd;
    uint16_t fail_seq;
    uint32_t notif_count;
    uint32_t notif_other;
    uint8_t last_notif;
    /* First IWM_INIT_OTHER_LOG unrecognised notifications:
     * cmd | group << 8 | payload length << 16. */
#define IWM_INIT_OTHER_LOG 12U
    uint32_t other_log[IWM_INIT_OTHER_LOG];
    uint32_t phy_cfg;
    uint32_t tx_ant;
    uint32_t flow_trigger;
    uint32_t event_trigger;
    int32_t mcc_rc;
    uint16_t mcc_len;
    uint8_t mcc_resp[32];
    uint8_t scratch[8U + IWM_NVM_CHUNK + 8U];
};

void iwm_init_fw_init(struct iwm_init_fw *st, struct iwm_adapter *a,
                      struct iwm_rx_queue *rxq, struct iwm_tx_transport *tx);
void iwm_init_fw_free(struct iwm_init_fw *st);
/* Run after a validated INIT ALIVE with the scheduler started. */
int iwm_init_fw_run(struct iwm_init_fw *st);
/* Linux iwl_trans_pcie_stop_device(): quiesce DMA and reset the NIC while
 * keeping the staged image, so the runtime image can be loaded next. */
int iwm_init_fw_stop(struct iwm_init_fw *st);
/* Section data or NULL when absent. */
const uint8_t *iwm_init_fw_nvm(const struct iwm_init_fw *st, unsigned type,
                               uint16_t *length);
#endif
