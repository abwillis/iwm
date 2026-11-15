/* os2/iwm_d32.h -- Drv32 glue shared by the strategy and probe files. */
#ifndef IWM_D32_H
#define IWM_D32_H

#include "Dev32lib.h"

int iwm_d32_m1(void);

/* Mapped CSR window left in place by a successful iwm_d32_m1(), or 0. */
volatile unsigned long *iwm_d32_csr(void);

/* Create the adapter and bind the firmware store; call once M1 passed. */
int iwm_d32_fw_attach(void);

/* STRATEGY_GENIOCTL for category 0x91 (firmware loader). */
void iwm_d32_ioctl(REQPACKET *prp);

/* Filled by iwm_d32_m1(); reported by the strategy READ handler. */
extern volatile unsigned long iwm_m1_rc;
extern volatile unsigned long iwm_m1_hwrev;
extern volatile unsigned long iwm_m1_irq;
extern volatile unsigned long iwm_m1_irq_count;

#endif
