/* os2/iwm_d32_m1.c -- Drv32 PCI/MSI probe and persistent CSR/ISR anchor.
 * Firmware DMA is implemented by iwm_d32_fw.c after this probe succeeds.
 * Declarations: Dev32lib.h, Dev32Help.h, bsekee.h (OS2Stuff/Drv32). */
#include "Dev32lib.h"
#include "iwm_bar.h"
#include "iwm_d32.h"                                  /* M1 read-out */

#define IWM_VENDOR       0x8086
#define IWM_DEVICE       0xA370
#define CSR_HW_IF_CONFIG 0x000
#define CSR_INT          0x008
#define CSR_INT_MASK     0x00C
#define CSR_HW_REV       0x028
#define PCI_CMD          0x04
#define PCI_CMD_INTX_DIS 0x0400

/* Exported for the strategy READ handler.             M1 read-out */
volatile unsigned long iwm_m1_rc = 0xFFFFFFFFUL;
volatile unsigned long iwm_m1_hwrev;
volatile unsigned long iwm_m1_irq;
volatile unsigned long iwm_m1_irq_count;

static volatile ULONG *g_csr;
static ULONG g_irq;
static volatile ULONG g_last_cause;
static SpinLock_t g_lock;

volatile unsigned long *iwm_d32_csr(void)
{
    return g_csr;
}

void iwm_d32_lock(void)
{
    KernAcquireSpinLock(&g_lock);
}

void iwm_d32_unlock(void)
{
    KernReleaseSpinLock(&g_lock);
}

#pragma aux iwm_msi_isr parm [eax]
static int iwm_msi_isr(ULONG data)
{
    ULONG cause;

    (void)data;
    KernAcquireSpinLock(&g_lock);
    cause = g_csr ? g_csr[CSR_INT / 4] : 0;
    if (cause != 0 && cause != 0xFFFFFFFFUL)
        g_csr[CSR_INT / 4] = cause;          /* write-1-to-clear */
    g_last_cause = cause;
    ++iwm_m1_irq_count;                               /* M1 read-out */
    KernReleaseSpinLock(&g_lock);
    if (cause != 0 && cause != 0xFFFFFFFFUL)
        iwm_d32_irq_notify(cause);
    Dev32Help_EOI(g_irq);
    return 0; /* return-value meaning not documented in Dev32Help.h */
}

int iwm_d32_m1(void)
{
    PCI_DEVICEINFO di;
    USHORT bdf, cmd = 0;
    ULONG lo = 0, hi = 0, phys = 0, size = 0, nmsi = 0;
    UCHAR irq = 0;
    int msi, rc;

    bdf = PciFindDevice(IWM_VENDOR, IWM_DEVICE, 0);
    if (bdf == 0xFFFF) { dprintf(0, "iwm: device not found\n"); return 1; }
    if (PciGetDeviceInfo(bdf, &di)) return 2;
    dprintf(0, "iwm: bdf %x sub %x:%x rev %x pin %d line %d\n",
            bdf, di.subsystem_vendor, di.subsystem_device,
            di.prog_if_rev_id & 0xFF, di.ipin, di.irq);

    /* 1. MSI before anything else. */
    msi = PsdMsiCount(bdf);
    dprintf(0, "iwm: PsdMsiCount %d\n", msi);
    if (msi == 0 || msi == -1) return 3;

    /* 2. BAR0 from both dwords; refuse above 4 GiB. */
    PciReadConfig(bdf, 0x10, 4, &lo);
    PciReadConfig(bdf, 0x14, 4, &hi);
    rc = iwm_bar_decode(lo, hi, &phys);
    dprintf(0, "iwm: BAR0 %08lx:%08lx decode %d\n", hi, lo, rc);
    if (rc != IWM_OK) return 4;

    /* 3. Power, bus master, INTx off (MSI only). */
    if (PciSetPowerStateD0(&di)) return 5;
    if (PciSetBusMaster(&di)) return 6;
    PciReadConfig(bdf, PCI_CMD, 2, &cmd);
    PciWriteConfig(bdf, PCI_CMD, 2, cmd | PCI_CMD_INTX_DIS);

    /* 4. Map CSR window (16 KiB per lspci). */
    size = 0x4000;
    g_csr = (volatile ULONG *)MapPhysToLin(phys, size);
    if (!g_csr) return 7;
    iwm_m1_hwrev = g_csr[CSR_HW_REV / 4];             /* M1 read-out */
    dprintf(0, "iwm: HW_REV %08lx HW_IF_CONFIG %08lx\n",
            iwm_m1_hwrev, g_csr[CSR_HW_IF_CONFIG / 4]);
    if (iwm_m1_hwrev == 0xFFFFFFFFUL) { rc = 8; goto unmap; }

    /* 5. Keep the device silent, then allocate and hook the MSI. */
    g_csr[CSR_INT_MASK / 4] = 0;
    g_csr[CSR_INT / 4] = 0xFFFFFFFFUL;
    KernAllocSpinLock(&g_lock);
    nmsi = 1;
    if (PsdMsiAlloc(bdf, &nmsi, &irq) || nmsi == 0) { rc = 9; goto unmap; }
    g_irq = irq;
    iwm_m1_irq = g_irq;                               /* M1 read-out */
    if (Dev32Help_SetIRQ((void *)iwm_msi_isr, (USHORT)g_irq, 0, 0)) {
        rc = 10;
        goto release;
    }
    dprintf(0, "iwm: MSI irq %lu installed; count %lu (expect 0)\n",
            g_irq, iwm_m1_irq_count);                 /* M1 read-out */
    return 0;  /* M1 passes; teardown path needs a verified UnSetIRQ */

release:
    PsdMsiRelease(bdf);
unmap:
    D32UnMap((void *)g_csr);
    g_csr = 0;
    return rc;
}

