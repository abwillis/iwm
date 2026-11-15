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

/* Filled by iwm_d32_m1(); reported by the strategy READ handler. */
extern volatile unsigned long iwm_m1_rc;
extern volatile unsigned long iwm_m1_hwrev;
extern volatile unsigned long iwm_m1_irq;
extern volatile unsigned long iwm_m1_irq_count;

#endif


