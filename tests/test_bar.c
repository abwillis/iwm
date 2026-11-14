/* SPDX-License-Identifier: GPL-2.0-only */
/* BAR decoding, including the real Intel 8086:a370 values from lspci. */
#include "assert.h"
#include "stdio.h"
#include "iwm_bar.h"

int main(void)
{
    uint32_t v = 0;

    /* lspci: "Memory at 604b10c000 (64-bit, non-prefetchable) [size=16K]".
     * Low dword 0x4B10C004 (64-bit type), high dword 0x00000060. The low
     * dword alone would name physical 0x4B10C000, which is wrong. */
    assert(iwm_bar_decode(0x4B10C004UL, 0x60UL, &v) == IWM_ERANGE);

    /* The same card with firmware placing the BAR below 4 GiB. */
    assert(iwm_bar_decode(0xF7E00004UL, 0UL, &v) == IWM_OK);
    assert(v == 0xF7E00000UL);
    assert(iwm_bar_decode(0xF7E00000UL, 0UL, &v) == IWM_OK);
    assert(v == 0xF7E00000UL);

    /* Prefetchable 32-bit BAR keeps only the address bits. */
    assert(iwm_bar_decode(0xF7E0000CUL, 0UL, &v) == IWM_OK);

    /* I/O BAR, reserved types and an unimplemented BAR are rejected. */
    assert(iwm_bar_decode(0x0000E001UL, 0UL, &v) == IWM_EINVAL);
    assert(iwm_bar_decode(0xF7E00002UL, 0UL, &v) == IWM_EINVAL);
    assert(iwm_bar_decode(0xF7E00006UL, 0UL, &v) == IWM_EINVAL);
    assert(iwm_bar_decode(0x00000004UL, 0UL, &v) == IWM_EINVAL);
    assert(iwm_bar_decode(0x4B10C004UL, 0x60UL, NULL) == IWM_EINVAL);

    /* Size read-back after writing all ones: 16 KiB for both layouts. */
    assert(iwm_bar_size(0xFFFFC004UL, 0xFFFFFFFFUL, 1, &v) == IWM_OK);
    assert(v == 0x4000UL);
    assert(iwm_bar_size(0xFFFFC000UL, 0UL, 0, &v) == IWM_OK);
    assert(v == 0x4000UL);
    /* A 64-bit BAR whose high dword does not read back as all ones is at
     * least 4 GiB. */
    assert(iwm_bar_size(0xFFFFC004UL, 0xFFFFFFF0UL, 1, &v) == IWM_ERANGE);
    assert(iwm_bar_size(0x0000000FUL, 0UL, 0, &v) == IWM_EINVAL);

    puts("bar tests passed");
    return 0;
}
