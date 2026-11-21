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
#define CSR_FH_INT_STATUS 0x010
#define IWM_INT_FH_TX    0x08000000UL
#define IWM_FH_INT_TX_MASK 0x00000003UL
/* Linux iwl-csr.h: CSR_INT_BIT_FH_RX and CSR_INT_BIT_SW_RX are summaries of
 * CSR_FH_INT_STATUS RX_CHNL0 (16), RX_CHNL1 (17) and HI_PRIOR (30)
 * (CSR_FH_INT_RX_MASK); iwl_pcie_irq_handler() clears them on every RX. */
#define IWM_INT_FH_RX    0x80000000UL
#define IWM_INT_SW_RX    0x00000008UL
#define IWM_FH_INT_RX_MASK 0x40030000UL
#define PCI_CMD          0x04
#define PCI_CMD_INTX_DIS 0x0400
/* MSI capability at 0xD0 on this 8086:a370 (lspci "Capabilities: [d0] MSI",
 * 64-bit): control at +2, address low +4, address high +8, data +0x0C. */
#define PCI_MSI_CAP      0xD0
/* Value iwm_msi_isr() returns for an interrupt it handled.  A/B test on the
 * 9560 (d7 logs): with 0 the kernel cleared MSI Enable after exactly 11
 * handler entries (msictl 0081 -> 0080, INT/FH latched, no more MSIs); with 1
 * it never did through INIT, the runtime load and every scan in the log
 * (146 entries).
 * The PDD Reference and driver-lifecycle.md describe the contract as: claimed
 * = carry clear, unclaimed = carry set, and the kernel masks an IRQ after
 * more than 10 unclaimed in a row.  So 1 means handled; an entry that found
 * no cause always returns the opposite value (unclaimed).  Build with
 * IWM_DEFS=-dIWM_ISR_HANDLED=0 to reproduce the old behaviour. */
#ifndef IWM_ISR_HANDLED
#define IWM_ISR_HANDLED  1
#endif

/* Exported for the strategy READ handler.             M1 read-out */
volatile unsigned long iwm_m1_rc = 0xFFFFFFFFUL;
volatile unsigned long iwm_m1_hwrev;
volatile unsigned long iwm_m1_irq;
volatile unsigned long iwm_m1_irq_count;

static volatile ULONG *g_csr;
static ULONG g_irq;
static USHORT g_bdf;
static volatile ULONG g_mask_shadow;
static volatile ULONG g_isr_entries;
static volatile ULONG g_isr_zero;
static volatile ULONG g_isr_gone;
static ULONG g_msi_rearms;
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
    ++g_isr_entries;
    cause = 0;
    if (g_csr) {
        /* Linux iwl_pcie_isr pattern: mask, read and acknowledge, then
         * restore the mask. Restoring it makes the device send a fresh MSI
         * for any cause that latched while the handler ran; without this a
         * cause such as ALIVE or SW_ERR can stay pending with no interrupt. */
        g_csr[CSR_INT_MASK / 4] = 0;
        cause = g_csr[CSR_INT / 4];
        if (cause != 0 && cause != 0xFFFFFFFFUL) {
            g_csr[CSR_INT / 4] = cause;      /* write-1-to-clear */
            if (cause & IWM_INT_FH_TX)       /* source of the FH_TX summary */
                g_csr[CSR_FH_INT_STATUS / 4] = IWM_FH_INT_TX_MASK;
            /* Same two-level latch for RX: without this the FH_RX summary
             * stays set after the first RX packet (log: fh 40010000,
             * int 80000001) and no further MSI is generated. */
            if (cause & (IWM_INT_FH_RX | IWM_INT_SW_RX))
                g_csr[CSR_FH_INT_STATUS / 4] = IWM_FH_INT_RX_MASK;
        }
        if (cause == 0)
            ++g_isr_zero;
        if (cause == 0xFFFFFFFFUL)
            ++g_isr_gone;
        else
            g_csr[CSR_INT_MASK / 4] = g_mask_shadow;
    }
    g_last_cause = cause;
    if (cause != 0 && cause != 0xFFFFFFFFUL)
        ++iwm_m1_irq_count;                           /* M1 read-out */
    KernReleaseSpinLock(&g_lock);
    if (cause != 0 && cause != 0xFFFFFFFFUL)
        iwm_d32_irq_notify(cause);
    Dev32Help_EOI(g_irq);
    /* See IWM_ISR_HANDLED. */
    return (cause != 0 && cause != 0xFFFFFFFFUL) ? IWM_ISR_HANDLED
                                                  : !IWM_ISR_HANDLED;
}

void iwm_d32_set_int_mask(unsigned long mask)
{
    KernAcquireSpinLock(&g_lock);
    g_mask_shadow = mask;
    if (g_csr)
        g_csr[CSR_INT_MASK / 4] = mask;
    KernReleaseSpinLock(&g_lock);
}

void iwm_d32_int_dump(const char *tag)
{
    static USHORT ctl, data, cmd;   /* static: see W112 note */
    static ULONG lo, hi;
    ULONG ci = 0, cm = 0, fh = 0;
    if (g_csr) {
        ci = g_csr[CSR_INT / 4];
        cm = g_csr[CSR_INT_MASK / 4];
        fh = g_csr[CSR_FH_INT_STATUS / 4];
    }
    ctl = data = cmd = 0;
    lo = hi = 0;
    PciReadConfig(g_bdf, PCI_CMD, 2, &cmd);
    PciReadConfig(g_bdf, PCI_MSI_CAP + 2, 2, &ctl);
    PciReadConfig(g_bdf, PCI_MSI_CAP + 4, 4, &lo);
    PciReadConfig(g_bdf, PCI_MSI_CAP + 8, 4, &hi);
    PciReadConfig(g_bdf, PCI_MSI_CAP + 0x0C, 2, &data);
    dprintf(0,
        "iwm: intdump %s isr %lu zero %lu gone %lu msi %lu int %08lx mask %08lx fh %08lx shadow %08lx cmd %04x msictl %04x addr %08lx:%08lx data %04x\n",
        tag, g_isr_entries, g_isr_zero, g_isr_gone, iwm_m1_irq_count,
        ci, cm, fh, g_mask_shadow, cmd, ctl, hi, lo, data);
}

unsigned iwm_d32_msi_ctl(void)
{
    static USHORT ctl;   /* static: see W112 note */
    ctl = 0;
    PciReadConfig(g_bdf, PCI_MSI_CAP + 2, 2, &ctl);
    return ctl;
}

int iwm_d32_msi_rearm(const char *tag)
{
    static USHORT before, after;   /* static: see W112 note */
    before = after = 0;
    PciReadConfig(g_bdf, PCI_MSI_CAP + 2, 2, &before);
    if (before & 0x0001U)
        return 0;
    PciWriteConfig(g_bdf, PCI_MSI_CAP + 2, 2, before | 0x0001U);
    PciReadConfig(g_bdf, PCI_MSI_CAP + 2, 2, &after);
    ++g_msi_rearms;
    dprintf(0, "iwm: msi rearm %s ctl %04x>%04x count %lu isr %lu msi %lu\n",
            tag, before, after, g_msi_rearms, g_isr_entries,
            iwm_m1_irq_count);
    return 1;
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
    g_bdf = bdf;
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
    rc = iwm_d32_rm_create_adapter(phys, size, (USHORT)g_irq,
                                   (USHORT)di.ipin, bdf);
    if (rc != IWM_OK) {
        rc = 10;
        goto release;
    }
    if (Dev32Help_SetIRQ((void *)iwm_msi_isr, (USHORT)g_irq, 0, 0)) {
        rc = 11;
        goto unregister;
    }
    dprintf(0, "iwm: MSI irq %lu installed; count %lu (expect 0) isr-handled %d\n",
            g_irq, iwm_m1_irq_count, IWM_ISR_HANDLED);  /* M1 read-out */
    {
        static USHORT msi_ctl, msi_data, pcmd;   /* static: see W112 note */
        static ULONG msi_lo, msi_hi;
        PciReadConfig(bdf, PCI_MSI_CAP + 2, 2, &msi_ctl);
        PciReadConfig(bdf, PCI_MSI_CAP + 4, 4, &msi_lo);
        PciReadConfig(bdf, PCI_MSI_CAP + 8, 4, &msi_hi);
        PciReadConfig(bdf, PCI_MSI_CAP + 0x0C, 2, &msi_data);
        PciReadConfig(bdf, PCI_CMD, 2, &pcmd);
        dprintf(0, "iwm: msi ctl %04x addr %08lx:%08lx data %04x cmd %04x\n",
                msi_ctl, msi_hi, msi_lo, msi_data, pcmd);
    }
    return 0;  /* M1 passes; teardown path needs a verified UnSetIRQ */

unregister:
    iwm_d32_rm_destroy(0);
release:
    PsdMsiRelease(bdf);
unmap:
    D32UnMap((void *)g_csr);
    g_csr = 0;
    return rc;
}



