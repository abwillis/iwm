/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef IWM_BAR_H
#define IWM_BAR_H

#include "iwm_types.h"

/*
 * PCI BAR decoding for the CSR window. The OS/2 DevHelp physical-address
 * services take a 32-bit physical address (ULONG PhysAddr), so a memory BAR
 * that firmware placed at or above 4 GiB cannot be mapped. A 64-bit BAR must
 * therefore be decoded from BOTH dwords: truncating it to the low dword
 * silently selects an unrelated physical address.
 */
#define IWM_BAR_IO          0x1UL
#define IWM_BAR_TYPE_MASK   0x6UL
#define IWM_BAR_TYPE_32     0x0UL
#define IWM_BAR_TYPE_64     0x4UL
#define IWM_BAR_FLAGS_MASK  0xFUL

/* Decode BAR0. lo is the BAR dword; hi is the next dword and is consulted
 * only for a 64-bit BAR (pass 0 otherwise). Returns:
 *   IWM_OK      *phys is a usable 32-bit physical base
 *   IWM_ERANGE  64-bit BAR placed at or above 4 GiB (unreachable)
 *   IWM_EINVAL  I/O BAR, reserved type, or unimplemented (zero) BAR
 */
int iwm_bar_decode(uint32_t lo, uint32_t hi, uint32_t *phys);

/* Size of a memory BAR from the read-back taken after writing all ones to
 * it (the caller restores the original value). Returns IWM_ERANGE when the
 * window is 4 GiB or larger, IWM_EINVAL for an unimplemented BAR.
 */
int iwm_bar_size(uint32_t sized_lo, uint32_t sized_hi, int is64,
                 uint32_t *size);

#endif
