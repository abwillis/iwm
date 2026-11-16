/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef IWM_HW_H
#define IWM_HW_H

#include "iwm_types.h"
#include "iwm_pci_ids.h"
#include "iwm_fw_proto.h"

struct iwm_scan_cache;

#define IWM_TX_RING_COUNT        256U
#define IWM_RX_LEGACY_RING_COUNT 256U
#define IWM_RX_MQ_RING_COUNT     512U
#define IWM_RX_BUFFER_SIZE       4096U
#define IWM_ICT_SIZE             4096U

struct iwm_dma_block {
    void *vaddr;          /* aligned address exposed to the hardware layer */
    uint32_t paddr;       /* aligned DMA32 physical address */
    uint32_t size;
    uint32_t alignment;
    void *alloc_vaddr;    /* OS allocation base, used when alignment shifted */
    uint32_t alloc_size;
};

struct iwm_os_ops {
    uint32_t (*mmio_read32)(void *context, uint32_t offset);
    void (*mmio_write32)(void *context, uint32_t offset, uint32_t value);
    void (*delay_us)(void *context, uint32_t usec);
    int (*dma_alloc)(void *context, uint32_t size, uint32_t alignment,
                     struct iwm_dma_block *block);
    void (*dma_free)(void *context, struct iwm_dma_block *block);
    void (*lock)(void *context);
    void (*unlock)(void *context);
    void (*log)(void *context, int level, const char *message);
};

enum iwm_adapter_state {
    IWM_ADAPTER_DOWN = 0,
    IWM_ADAPTER_PROBED,
    IWM_ADAPTER_FW_WAIT,
    IWM_ADAPTER_READY,
    IWM_ADAPTER_SCANNING,
    IWM_ADAPTER_ASSOCIATED,
    IWM_ADAPTER_ERROR
};

struct iwm_ring_plan {
    uint16_t tx_count;
    uint16_t rx_count;
    uint16_t tx_desc_size;
    uint16_t rx_desc_size;
    uint32_t tx_desc_bytes;
    uint32_t rx_desc_bytes;
};

struct iwm_adapter {
    const struct iwm_device_config *cfg;
    const struct iwm_os_ops *ops;
    void *os_context;
    uint32_t bar0;
    uint8_t irq;
    uint8_t lan_number;
    uint16_t state;
    uint32_t hw_rev;
    uint32_t pending_interrupts;
    uint32_t link_speed_kbps;
    int32_t rssi_dbm;
    uint32_t status_generation;
    uint32_t infrastructure_mode;
    uint32_t authentication_mode;
    uint32_t encryption_status;
    uint32_t privacy_filter;
    uint32_t power_mode;
    uint32_t network_type;
    uint8_t connected;
    uint8_t mac_valid;
    uint8_t mac[6];
    uint8_t bssid[6];
    uint8_t ssid[32];
    uint8_t ssid_length;
    struct iwm_scan_cache *scan_cache;
    struct iwm_fw_session firmware;
    struct iwm_ring_plan ring_plan;
    struct iwm_dma_block tx_desc;
    struct iwm_dma_block rx_desc;
    struct iwm_dma_block ict;
};

void iwm_adapter_init(struct iwm_adapter *adapter,
                      const struct iwm_os_ops *ops, void *context);
int iwm_adapter_probe(struct iwm_adapter *adapter, uint16_t vendor,
                      uint16_t device, uint32_t bar0, uint8_t irq);
int iwm_prepare_card_hw(struct iwm_adapter *adapter);
int iwm_stop_device(struct iwm_adapter *adapter);
int iwm_grab_nic_access(struct iwm_adapter *adapter);
void iwm_release_nic_access(struct iwm_adapter *adapter);
int iwm_plan_rings(const struct iwm_adapter *adapter,
                   struct iwm_ring_plan *plan);
int iwm_allocate_rings(struct iwm_adapter *adapter);
void iwm_free_rings(struct iwm_adapter *adapter);
/* Called from deferred interrupt work when the runtime ALIVE notification
 * arrives; moves firmware LOADING -> RUNNING and the adapter to READY. */
int iwm_adapter_fw_alive(struct iwm_adapter *adapter);
/* Read the factory MAC address from the CSR window at csr_base (Linux
 * mac_addr_from_csr, 0x380 for the 9000 family): STRAP registers first, the
 * OTP registers when STRAP is not a valid unicast address. On success the
 * address is installed with iwm_adapter_set_mac(). raw[4] receives the four
 * register values (STRAP0, STRAP1, OTP0, OTP1) for diagnostics; may be NULL. */
int iwm_adapter_read_csr_mac(struct iwm_adapter *adapter, uint32_t csr_base,
                             uint32_t raw[4]);
/* Bind the OS-owned physical firmware store. A non-NULL store may only be
 * bound while no image is in flight; NULL always unbinds and resets the
 * firmware session (used by detach after iwm_stop_device). */
int iwm_adapter_set_fw_store(struct iwm_adapter *adapter,
                             struct iwm_blob *store);
int iwm_adapter_set_mac(struct iwm_adapter *adapter, const uint8_t mac[6]);

#endif



