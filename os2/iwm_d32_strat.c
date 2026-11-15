/* os2/iwm_d32_strat.c -- minimal Drv32 entry point for the M1 probe.
 * Pattern: Drv32 Driver.c sample and the GPL NDIS strat.c (Azarewicz).
 * Draft; not compiled. */
#include "Dev32lib.h"
#include "iwm_d32.h"

/* XWLAN and IWMFWLD.EXE open \DEV\WRND32$. Only one driver may own it. */
char cDevName[] = "WRND32$";

static int g_opened;   /* open count: XWLAN and IWMFWLD.EXE both open this device */
static int g_status_pending;   /* READ returns one status line per open */

static void StratInit(REQPACKET *prp)
{
    int rc;

    UtSetDriverName(cDevName);
    D32g_DbgLevel = 2;
    D32g_DbgFlags |= D32DBGF_TIMESTAMP;
    dprintf(0, "iwm: M1 probe build, args '%s'\n",
            Far16ToFlat(prp->init_in.szArgs));

    rc = iwm_d32_m1();
    iwm_m1_rc = (unsigned long)rc;
    dprintf(0, "iwm: M1 result %d\n", rc);

    if (rc == 0) {
        rc = iwm_d32_fw_attach();
        dprintf(0, "iwm: fw attach %d\n", rc);
    }

    /* Stay loaded whatever the result, so the log can be read through
     * \DEV\WRND32$ and a hooked MSI is never left without its handler. */
    prp->usStatus = RPDONE;
}

static void StratRead(REQPACKET *prp)
{
    void *lin;

    if (Dev32Help_PhysToLin(prp->io.ulAddress, prp->io.usCount, &lin)) {
        prp->io.usCount = 0;
        prp->usStatus |= RPERR_GENERAL;
        return;
    }
    /* The status line is written once per open. Writing it on every read
     * refills the buffer that dCopyToUser() drains, so a reader such as
     * "type wrnd32$" never sees the end of the data. */
    if (g_status_pending) {
        g_status_pending = 0;
        dprintf(0, "iwm: rc=%lu hwrev=%08lx irq=%lu ints=%lu\n",
                iwm_m1_rc, iwm_m1_hwrev, iwm_m1_irq, iwm_m1_irq_count);
    }
    prp->io.usCount = (USHORT)dCopyToUser(lin, prp->io.usCount);
}

void StrategyHandler(REQPACKET *prp)
{
    prp->usStatus = RPDONE;

    switch (prp->bCommand) {
    case STRATEGY_INIT:
        StratInit(prp);
        break;
    case STRATEGY_OPEN:
        ++g_opened;
        g_status_pending = 1;
        dprintf(0, "iwm: open, count %d\n", g_opened);
        break;
    case STRATEGY_CLOSE:
        if (g_opened > 0)
            --g_opened;
        dprintf(0, "iwm: close, count %d\n", g_opened);
        break;
    case STRATEGY_READ:
        StratRead(prp);
        break;
    case STRATEGY_GENIOCTL:
        /* 0x91 firmware loader only; GenMAC (0x99) is not served yet. */
        iwm_d32_ioctl(prp);
        break;
    case STRATEGY_DEINSTALL:
    case STRATEGY_INITCOMPLETE:
    case STRATEGY_SAVERESTORE:
        break;
    default:
        prp->usStatus = RPDONE | RPERR | RPERR_BADCOMMAND;
        break;
    }
}
