/* os2/iwm_d32.h -- Drv32 glue shared by the strategy and probe files. */
#ifndef IWM_D32_H
#define IWM_D32_H

#include "Dev32lib.h"
/* IWM_OK and the other IWM_* status codes returned by the calls below. */
#include "iwm_types.h"

int iwm_d32_m1(void);

/* Register the driver and claimed PCI/MSI resources with Resource Manager. */
int iwm_d32_rm_create_driver(void);
int iwm_d32_rm_create_adapter(unsigned long bar0, unsigned long bar_size,
                              unsigned short irq, unsigned short pin,
                              unsigned short bdf);
void iwm_d32_rm_destroy(int destroy_driver);

/* Mapped CSR window left in place by a successful iwm_d32_m1(), or 0. */
volatile unsigned long *iwm_d32_csr(void);

/* Create the adapter and bind the firmware store; call once M1 passed. */
int iwm_d32_fw_attach(void);

/* STRATEGY_GENIOCTL for firmware-loader category 0x91 and GenMAC 0x99. */
void iwm_d32_ioctl(REQPACKET *prp);

/* Shared ring-0 lock and ISR-to-firmware-loader notification path. */
void iwm_d32_lock(void);
void iwm_d32_unlock(void);
void iwm_d32_irq_notify(unsigned long cause);
/* Transmit one Ethernet II frame (14-byte header + payload, at most 1514
 * bytes) on data queue 4 while associated.  Entry point for the NDIS MAC
 * layer; returns IWM_ESTATE when not associated. */
int iwm_d32_data_tx(const void *eth, unsigned short len);
/* Bind the Drv32 NDIS 2.0.1 MAC callbacks to the generic IWM adapter. */
struct iwm_ndis_context;
struct iwm_adapter;
void iwm_d32_ndis_bind(struct iwm_ndis_context *ndis,
                       struct iwm_adapter *adapter);
/* One log line of NDIS open/transmit/indication counters. */
void iwm_d32_ndis_log(const char *why);
/* Receive-indication outcomes: how often the protocol accepted, declined or
 * failed a frame, and the last NdisRcvLkAhead return code. */
void iwm_d32_ndis_log_rx(const char *why);
/* CSR_INT_MASK writes from task level go through here: the ISR restores
 * this shadow instead of the value it read on entry, so a mask the task
 * changed while the ISR ran is not overwritten with a stale one. */
void iwm_d32_set_int_mask(unsigned long mask);
/* One log line: ISR entries (all / zero cause / all-ones), MSI count,
 * CSR_INT, CSR_INT_MASK, CSR_FH_INT_STATUS, shadow mask, PCI command and
 * MSI capability (control, address, data). */
void iwm_d32_int_dump(const char *tag);
/* MSI Enable (control bit 0) was found cleared during the INIT command
 * phase (log: msictl 0081 at initload, 0080 at initseq) with no driver
 * write to config space.  If it is clear, set it again and log the
 * before/after control word; returns 1 when a re-arm was needed. */
int iwm_d32_msi_rearm(const char *tag);
/* MSI capability control word, bit 0 = MSI Enable (one config read). */
unsigned iwm_d32_msi_ctl(void);

/* Filled by iwm_d32_m1(); reported by the strategy READ handler. */
extern volatile unsigned long iwm_m1_rc;
extern volatile unsigned long iwm_m1_hwrev;
extern volatile unsigned long iwm_m1_irq;
extern volatile unsigned long iwm_m1_irq_count;

#endif


