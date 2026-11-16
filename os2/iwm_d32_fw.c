/* os2/iwm_d32_fw.c -- ring-0 firmware staging, category 0x91 IOCTL,
 * and M2 FH service-channel INIT loader for IWMFWLD.EXE.
 * Declarations: Dev32lib.h, Dev32Help.h, strategy.h (OS2Stuff/Drv32). */
#include "Dev32lib.h"
#include "iwm_hw.h"
#include "iwm_fw_loader.h"
#include "iwm_rx.h"
#include "iwm_os2.h"
#include "iwm_d32.h"

#define IWM_VENDOR  0x8086
#define IWM_DEVICE  0xA370
/* Linux cfg/9000.c mac_addr_from_csr. */
#define IWM_9000_MAC_CSR_BASE 0x380UL

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
    udelay((ULONG)us);
}

static int d_dma_alloc(void *c, uint32_t size, uint32_t alignment,
                       struct iwm_dma_block *block)
{
    ULONG raw_phys, total, shift;
    void *raw;

    (void)c;
    if (!block || size == 0 || alignment == 0 ||
        (alignment & (alignment - 1U)) ||
        size > 0xFFFFFFFFUL - (alignment - 1U))
        return IWM_EINVAL;
    memset(block, 0, sizeof(*block));
    total = (ULONG)size + (ULONG)alignment - 1UL;
    raw = D32AllocMem(total, &raw_phys, 0);
    if (!raw)
        return IWM_ENOMEM;
    shift = (0UL - raw_phys) & ((ULONG)alignment - 1UL);
    block->alloc_vaddr = raw;
    block->alloc_size = total;
    block->vaddr = (uint8_t *)raw + shift;
    block->paddr = raw_phys + shift;
    block->size = size;
    block->alignment = alignment;
    if (iwm_dma32_check(block->paddr, size, alignment) != IWM_OK) {
        D32FreeMem(raw);
        memset(block, 0, sizeof(*block));
        return IWM_ERANGE;
    }
    memset(block->vaddr, 0, size);
    return IWM_OK;
}

static void d_dma_free(void *c, struct iwm_dma_block *block)
{
    void *raw;

    (void)c;
    if (!block)
        return;
    raw = block->alloc_vaddr ? block->alloc_vaddr : block->vaddr;
    if (raw)
        D32FreeMem(raw);
    memset(block, 0, sizeof(*block));
}

static void d_lock(void *c)
{
    (void)c;
    iwm_d32_lock();
}

static void d_unlock(void *c)
{
    (void)c;
    iwm_d32_unlock();
}

static const struct iwm_os_ops g_ops = {
    d_rd, d_wr, d_delay, d_dma_alloc, d_dma_free, d_lock, d_unlock, 0
};

static struct iwm_adapter g_ad;
static struct iwm_scan_cache g_scan;
static struct iwm_blob g_store;
static struct iwm_fw_loader g_loader;
static struct iwm_rx_queue g_rxq;
static int g_attached;

int iwm_d32_fw_attach(void)
{
    int rc;

    if (g_attached)
        return IWM_EBUSY;
    if (!iwm_d32_csr())
        return IWM_ENODEV;
    iwm_adapter_init(&g_ad, &g_ops, 0);
    iwm_genmac_bind_scan_cache(&g_ad, &g_scan);
    rc = iwm_adapter_probe(&g_ad, IWM_VENDOR, IWM_DEVICE, 0,
                           (uint8_t)iwm_m1_irq);
    if (rc != IWM_OK)
        return rc;
    {
        static uint32_t raw[4];   /* static: see W112 note */
        int mrc = iwm_adapter_read_csr_mac(&g_ad, IWM_9000_MAC_CSR_BASE, raw);
        dprintf(0, "iwm: csr mac rc %d strap %08lx %08lx otp %08lx %08lx "
                "addr %02x:%02x:%02x:%02x:%02x:%02x\n", mrc,
                (ULONG)raw[0], (ULONG)raw[1], (ULONG)raw[2], (ULONG)raw[3],
                g_ad.mac[0], g_ad.mac[1], g_ad.mac[2],
                g_ad.mac[3], g_ad.mac[4], g_ad.mac[5]);
    }
    iwm_rx_init(&g_rxq, &g_ad);
    rc = iwm_rx_alloc(&g_rxq);
    if (rc != IWM_OK) {
        dprintf(0, "iwm: rx alloc rc %d\n", rc);
        return rc;
    }
    dprintf(0, "iwm: rxq free %08lx used %08lx status %08lx buffers %08lx/%lu\n",
            (ULONG)g_rxq.free_desc.paddr, (ULONG)g_rxq.used_desc.paddr,
            (ULONG)g_rxq.status.paddr, (ULONG)g_rxq.buffers.paddr,
            (ULONG)g_rxq.buffers.size);
    iwm_blob_bind(&g_store, &g_store_ops, 0);
    rc = iwm_adapter_set_fw_store(&g_ad, &g_store);
    if (rc == IWM_OK) {
        iwm_fw_loader_init(&g_loader, &g_ad);
        iwm_fw_loader_bind_rx(&g_loader, &g_rxq);
        g_attached = 1;
    } else
        iwm_rx_free(&g_rxq);
    return rc;
}

void iwm_d32_irq_notify(unsigned long cause)
{
    if (g_attached)
        iwm_fw_loader_note_irq(&g_loader, (uint32_t)cause);
}

static int start_init_firmware(void)
{
    int rc;

    rc = iwm_fw_loader_start(&g_loader, IWM_FW_SEC_INIT);
    dprintf(0,
        "iwm: init load rc %d stage %lu sections %lu bytes %lu dst %08lx aliveirq %lu aliventf %lu csr %08lx fh %08lx load %08lx ints %lu rx %lu bad %lu closed %lu\n",
        rc, (ULONG)g_loader.stage, (ULONG)g_loader.loaded_sections,
        (ULONG)g_loader.loaded_bytes, (ULONG)g_loader.last_dst,
        (ULONG)g_loader.alive_interrupt, (ULONG)g_loader.alive_notification,
        (ULONG)g_loader.last_csr_int,
        (ULONG)g_loader.last_fh_int, (ULONG)g_loader.last_load_status,
        (ULONG)iwm_m1_irq_count, (ULONG)g_rxq.packets,
        (ULONG)g_rxq.malformed, (ULONG)g_rxq.last_closed);
    if (g_loader.alive_notification)
        dprintf(0, "iwm: alive status %04x flags %04x lmac %08lx.%08lx\n",
                g_rxq.alive.status, g_rxq.alive.flags,
                (ULONG)g_rxq.alive.lmac_major,
                (ULONG)g_rxq.alive.lmac_minor);
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
    static struct iwm_os2_ioctl req;   /* static: see W112 note */
    void *data = 0;
    ULONG *parm = 0;
    UCHAR cat = prp->ioctl.bCategory;
    int rc;

    if (cat != IWM_IOCTL_CATEGORY && cat != GENMAC_CATEGORY) {
        prp->usStatus = RPDONE | RPERR_BADCOMMAND;
        return;
    }
    if (!g_attached) {
        dprintf(0, "iwm: ioctl cat %x before attach\n", (int)cat);
        prp->usStatus = RPDONE | RPERR_NOTREADY;
        return;
    }
    memset(&req, 0, sizeof(req));
    req.category = cat;
    req.function = prp->ioctl.bFunction;

    if (prp->ioctl.usDataLen) {
        data = Far16ToFlat(prp->ioctl.pvData);
        if (!data) {
            prp->usStatus = RPDONE | RPERR_PARAMETER;
            return;
        }
    }
    req.data = data;
    req.data_length = prp->ioctl.usDataLen;

    /* GenMAC: OID in parameters[0]; bytes done and NDIS status come back
     * in parameters[2] and [3]. */
    if (cat == GENMAC_CATEGORY) {
        if (prp->ioctl.usParmLen < sizeof(req.parameters)) {
            dprintf(0, "iwm: genmac fn %x parm len %u too short\n",
                    (int)req.function, (unsigned)prp->ioctl.usParmLen);
            prp->usStatus = RPDONE | RPERR_PARAMETER;
            return;
        }
        parm = (ULONG *)Far16ToFlat(prp->ioctl.pvParm);
        if (!parm) {
            prp->usStatus = RPDONE | RPERR_PARAMETER;
            return;
        }
        memcpy(req.parameters, parm, sizeof(req.parameters));
    }

    rc = iwm_os2_ioctl_dispatch(&g_ad, &req);

    if (parm)
        memcpy(parm, req.parameters, sizeof(req.parameters));
    if (req.data_actual && req.data_actual <= prp->ioctl.usDataLen)
        prp->ioctl.usDataLen = (USHORT)req.data_actual;

    if (cat == GENMAC_CATEGORY) {
        dprintf(0, "iwm: genmac fn %x oid %08lx len %lu rc %d done %lu st %08lx\n",
                (int)req.function, (ULONG)req.parameters[0],
                (ULONG)req.data_length, rc, (ULONG)req.parameters[2],
                (ULONG)req.parameters[3]);
    } else {
        if (req.function == IWM_IOCTL_FW_COMMIT) {
            log_commit(rc);
            if (rc == IWM_OK && g_ad.firmware.state == IWM_FW_LOADING)
                rc = start_init_firmware();
        }
        if (req.function != IWM_IOCTL_FW_DATA || rc != IWM_OK)
            dprintf(0, "iwm: fw fn %d len %lu rc %d state %d accepted %lu\n",
                    (int)req.function, (ULONG)req.data_length, rc,
                    (int)g_ad.firmware.state, (ULONG)g_ad.firmware.accepted);
    }

    /* A syntactically valid GenMAC request reports protocol failures through
     * parameters[3].  Returning an OS/2 request-packet error as well hides the
     * NDIS status from XWLAN and turns ADAPTER_NOT_READY into a generic IOCTL
     * failure. */
    if (cat == GENMAC_CATEGORY && req.parameters[3] != 0) {
        prp->usStatus = RPDONE;
        return;
    }

    switch (rc) {
    case IWM_OK:
        prp->usStatus = RPDONE;
        break;
    case IWM_EINVAL:
    case IWM_EOVERFLOW:
        prp->usStatus = RPDONE | RPERR_PARAMETER;
        break;
    case IWM_ENOTSUP:
        /* An unsupported OID is a normal GenMAC answer: the NDIS status in
         * parameters[3] says NOT_SUPPORTED, so let XWLAN read it. */
        prp->usStatus = cat == GENMAC_CATEGORY ? RPDONE
                                               : (RPDONE | RPERR_BADCOMMAND);
        break;
    default:
        prp->usStatus = RPDONE | RPERR_GENERAL;
        break;
    }
}


