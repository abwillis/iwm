/* os2/iwm_d32_fw.c -- ring-0 firmware staging store and the category 0x91
 * IOCTL entry for IWMFWLD.EXE. Draft; M2 hardware load is not included.
 * Declarations: Dev32lib.h, Dev32Help.h, strategy.h (OS2Stuff/Drv32). */
#include "Dev32lib.h"
#include "iwm_hw.h"
#include "iwm_os2.h"
#include "iwm_d32.h"

#define IWM_VENDOR  0x8086
#define IWM_DEVICE  0xA370

/* memcpy/memset come from Dev32lib.h (compiler intrinsics); the core files
 * use them the same way, so nothing is supplied here. */

/* Firmware store: fixed, contiguous memory with a flat address, so the image
 * is directly readable now and its physical address is known for the later
 * FH DMA step. */
static struct {
    void *base;
    ULONG phys;
    ULONG size;
} g_fw;

static int fws_resize(void *c, uint32_t n)
{
    (void)c;
    if (g_fw.base) {
        D32FreeMem(g_fw.base);
        g_fw.base = 0;
        g_fw.phys = 0;
        g_fw.size = 0;
    }
    if (n == 0)
        return IWM_OK;
    g_fw.base = D32AllocMem(n, &g_fw.phys, 0);
    if (!g_fw.base) {
        dprintf(0, "iwm: fw store alloc of %lu bytes failed\n", (ULONG)n);
        return IWM_ENOMEM;
    }
    g_fw.size = n;
    dprintf(0, "iwm: fw store %lu bytes at phys %08lx\n", (ULONG)n, g_fw.phys);
    return IWM_OK;
}

static int fws_write(void *c, uint32_t off, const uint8_t *d, uint16_t n)
{
    (void)c;
    if (!g_fw.base || off > g_fw.size || (ULONG)n > g_fw.size - off)
        return IWM_EOVERFLOW;
    memcpy((uint8_t *)g_fw.base + off, d, n);
    return IWM_OK;
}

static int fws_read(void *c, uint32_t off, uint8_t *d, uint16_t n)
{
    (void)c;
    if (!g_fw.base || off > g_fw.size || (ULONG)n > g_fw.size - off)
        return IWM_EOVERFLOW;
    memcpy(d, (uint8_t *)g_fw.base + off, n);
    return IWM_OK;
}

static void fws_reset(void *c)
{
    (void)c;
    if (g_fw.base)
        D32FreeMem(g_fw.base);
    g_fw.base = 0;
    g_fw.phys = 0;
    g_fw.size = 0;
}

static const struct iwm_blob_ops g_store_ops = {
    fws_resize, fws_write, fws_read, fws_reset
};

/* CSR access through the window iwm_d32_m1() mapped. */
static uint32_t d_rd(void *c, uint32_t off)
{
    volatile unsigned long *csr = iwm_d32_csr();

    (void)c;
    return csr ? csr[off / 4] : 0xFFFFFFFFUL;
}

static void d_wr(void *c, uint32_t off, uint32_t v)
{
    volatile unsigned long *csr = iwm_d32_csr();

    (void)c;
    if (csr)
        csr[off / 4] = v;
}

/* Nothing waits on hardware yet. Replace with udelay() once its prototype is
 * confirmed in Dev32lib.h, before iwm_prepare_card_hw() is ever called. */
static void d_delay(void *c, uint32_t us)
{
    (void)c;
    (void)us;
}

static const struct iwm_os_ops g_ops = { d_rd, d_wr, d_delay };

static struct iwm_adapter g_ad;
static struct iwm_blob g_store;
static int g_attached;

int iwm_d32_fw_attach(void)
{
    int rc;

    if (g_attached)
        return IWM_EBUSY;
    if (!iwm_d32_csr())
        return IWM_ENODEV;
    iwm_adapter_init(&g_ad, &g_ops, 0);
    rc = iwm_adapter_probe(&g_ad, IWM_VENDOR, IWM_DEVICE, 0,
                           (uint8_t)iwm_m1_irq);
    if (rc != IWM_OK)
        return rc;
    iwm_blob_bind(&g_store, &g_store_ops, 0);
    rc = iwm_adapter_set_fw_store(&g_ad, &g_store);
    if (rc == IWM_OK)
        g_attached = 1;
    return rc;
}

/* One summary line plus one line per section after COMMIT, or the exact
 * record the parser refused. Read it back with "type wrnd32$". */
static void log_commit(int rc)
{
    const struct iwm_fw_image *im = &g_ad.firmware.catalog;
    unsigned i;

    if (rc != IWM_OK) {
        dprintf(0, "iwm: parse refused rc %d reason %lu at %lu type %lu len %lu\n",
                rc, (ULONG)im->diag.reason, (ULONG)im->diag.pos,
                (ULONG)im->diag.type, (ULONG)im->diag.length);
        return;
    }
    dprintf(0, "iwm: image ver %lu build %08lx cpus %lu sections %lu init %lu rt %lu\n",
            (ULONG)im->version, (ULONG)im->build, (ULONG)im->num_cpus,
            (ULONG)im->section_count, (ULONG)im->init_count,
            (ULONG)im->runtime_count);
    for (i = 0; i < im->section_count; ++i)
        dprintf(0, "iwm: sec %lu type %lu dst %08lx len %lu flags %02lx\n",
                (ULONG)i, (ULONG)im->section[i].tlv_type,
                (ULONG)im->section[i].dst_addr,
                (ULONG)im->section[i].data_length,
                (ULONG)im->section[i].flags);
}

void iwm_d32_ioctl(REQPACKET *prp)
{
    struct iwm_os2_ioctl req;
    void *data = 0;
    int rc;

    if (prp->ioctl.bCategory != IWM_IOCTL_CATEGORY) {
        prp->usStatus = RPDONE | RPERR_BADCOMMAND;
        return;
    }
    if (!g_attached) {
        dprintf(0, "iwm: ioctl before attach\n");
        prp->usStatus = RPDONE | RPERR_NOTREADY;
        return;
    }
    memset(&req, 0, sizeof(req));
    req.category = prp->ioctl.bCategory;
    req.function = prp->ioctl.bFunction;
    if (prp->ioctl.usDataLen) {
        /* pvData is a 16:16 pointer into the caller's address space. */
        data = Far16ToFlat(prp->ioctl.pvData);
        if (!data) {
            prp->usStatus = RPDONE | RPERR_PARAMETER;
            return;
        }
    }
    req.data = data;
    req.data_length = prp->ioctl.usDataLen;

    rc = iwm_os2_ioctl_dispatch(&g_ad, &req);

    /* One line per DATA chunk would flood the debug buffer. */
    if (req.function != IWM_IOCTL_FW_DATA || rc != IWM_OK)
        dprintf(0, "iwm: fw fn %d len %lu rc %d state %d accepted %lu\n",
                (int)req.function, (ULONG)req.data_length, rc,
                (int)g_ad.firmware.state, (ULONG)g_ad.firmware.accepted);

    if (req.function == IWM_IOCTL_FW_COMMIT)
        log_commit(rc);

    switch (rc) {
    case IWM_OK:
        prp->usStatus = RPDONE;
        break;
    case IWM_EINVAL:
    case IWM_EOVERFLOW:
        prp->usStatus = RPDONE | RPERR_PARAMETER;
        break;
    case IWM_ENOTSUP:
        prp->usStatus = RPDONE | RPERR_BADCOMMAND;
        break;
    default:
        prp->usStatus = RPDONE | RPERR_GENERAL;
        break;
    }
}
