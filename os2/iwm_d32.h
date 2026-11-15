/* os2/iwm_d32.h -- Drv32 glue shared by the strategy and probe files. */
#ifndef IWM_D32_H
#define IWM_D32_H

int iwm_d32_m1(void);

/* Filled by iwm_d32_m1(); reported by the strategy READ handler. */
extern volatile unsigned long iwm_m1_rc;
extern volatile unsigned long iwm_m1_hwrev;
extern volatile unsigned long iwm_m1_irq;
extern volatile unsigned long iwm_m1_irq_count;

#endif
