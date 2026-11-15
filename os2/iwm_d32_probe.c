/* os2/iwm_d32_probe.c -- M1: find, decode, map, read CSR_HW_REV. */
#include <Dev32lib.h>
#include "iwm_bar.h"

#define IWM_CSR_HW_REV 0x028

int iwm_d32_probe(void)
{
    PCI_DEVICEINFO di;
    USHORT bdf;
    ULONG lo = 0, hi = 0, phys, rev;
    volatile ULONG *csr;
    int rc;

    bdf = PciFindDevice(0x8086, 0xA370, 0);
    if (bdf == 0xFFFF) { dprintf(0, "iwm: 8086:a370 not found\n"); return 1; }
    if (PciGetDeviceInfo(bdf, &di)) return 2;
    dprintf(0, "iwm: bdf %x sub %x:%x irq %d pin %d\n", bdf,
            di.subsystem_vendor, di.subsystem_device, di.irq, di.ipin);
    PciReadConfig(bdf, 0x10, 4, &lo);
    PciReadConfig(bdf, 0x14, 4, &hi);
    dprintf(0, "iwm: BAR0 lo %08lx hi %08lx\n", lo, hi);
    rc = iwm_bar_decode(lo, hi, &phys);
    if (rc != IWM_OK) { dprintf(0, "iwm: BAR0 unusable rc %d\n", rc); return 3; }
    if (PciSetPowerStateD0(&di)) return 4;
    csr = (volatile ULONG *)MapPhysToLin(phys, 0x4000);
    if (!csr) return 5;
    rev = csr[IWM_CSR_HW_REV / 4];
    dprintf(0, "iwm: CSR_HW_REV %08lx (Linux reported 0x312)\n", rev);
    D32UnMap((void *)csr);
    return rev == 0xFFFFFFFFUL ? 6 : 0;
}
