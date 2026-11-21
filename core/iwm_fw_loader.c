/* SPDX-License-Identifier: GPL-2.0-only */
/* Intel 8000/9000 secure firmware section loader.  The register sequence is
 * the FH service-channel flow used by iwlwifi/FreeBSD iwm; OS ownership,
 * allocation and interrupt delivery remain behind struct iwm_os_ops. */
#if !defined(IWM_RING0)
#include <string.h>
#endif
#include "iwm_fw_loader.h"
#include "iwm_rx.h"
#include "iwm_tx.h"

#define CSR_INT                         0x008UL
#define CSR_INT_MASK                    0x00cUL
#define CSR_FH_INT_STATUS               0x010UL
#define CSR_GP_CNTRL                    0x024UL
#define CSR_HW_IF_CONFIG_REG            0x000UL
/* Bits iwl_mvm_nic_config() owns: MAC dash/step, MAC_SI, RADIO_SI, PHY
 * type/dash/step. */
#define CSR_HW_IF_NIC_CONFIG_MASK       0x0000ff0fUL
#define FH_INT_TX_MASK                  0x00000003UL /* TX chnl 0/1 */
#define CSR_UCODE_DRV_GP1_CLR           0x05cUL
#define CSR_UCODE_SW_BIT_RFKILL         0x00000002UL
#define CSR_UCODE_DRV_GP1_CMD_BLOCKED   0x00000004UL
#define HBUS_TARG_PRPH_WADDR            0x444UL
#define HBUS_TARG_PRPH_RADDR            0x448UL
#define HBUS_TARG_PRPH_WDAT             0x44cUL
#define HBUS_TARG_PRPH_RDAT             0x450UL
#define HBUS_TARG_MEM_RADDR             0x40cUL
#define HBUS_TARG_MEM_RDAT              0x41cUL
#define HBUS_PRPH_DWORD                 (3UL << 24)
#define FH_UCODE_LOAD_STATUS            0x1af0UL
#define FH_TFDIB_CTRL0(ch)              (0x1900UL + 8UL * (ch))
#define FH_TFDIB_CTRL1(ch)              (0x1904UL + 8UL * (ch))
#define FH_TCSR_CONFIG(ch)              (0x1d00UL + 0x20UL * (ch))
#define FH_TCSR_BUF_STATUS(ch)           (0x1d08UL + 0x20UL * (ch))
#define FH_SRVC_SRAM_ADDR(ch)            (0x19c8UL + 4UL * ((ch) - 9UL))
#define FH_SRVC_CHNL                     9UL
#define FH_TFDIB_ADDR_HI_SHIFT           28U
#define FH_TCSR_PAUSE                    0x00000000UL
#define FH_TCSR_ENABLE                   0x80000000UL
#define FH_TCSR_CREDIT_DISABLE           0x00000000UL
#define FH_TCSR_HOST_ENDTFD              0x00100000UL
#define FH_TCSR_TFDB_VALID               0x00000003UL
#define FH_TCSR_TB_NUM_POS               20U
#define FH_TCSR_TB_IDX_POS               12U
#define RELEASE_CPU_RESET                0x300cUL
#define RELEASE_CPU_RESET_BIT            0x01000000UL
/* Linux trace of this card writes PRPH 0xa030b4 = 0x01010101 just before
 * RELEASE_CPU_RESET on every load (INIT and runtime).  Believed to be
 * WFPM_GP2 in iwl-prph.h; name not verified against the indexed source. */
#define WFPM_GP2                         0x00a030b4UL
#define WFPM_GP2_VALUE                   0x01010101UL
#define LMPM_SECURE_CPU2_HDR_ADDR         0x001e7cUL
#define LMPM_SECURE_CPU2_HDR_MEM_SPACE    0x00420400UL
#define LMPM_CHICK                       0x00a01ff8UL
/* Linux iwl_mvm_load_ucode_wait_alive() failure diagnostics (iwl-prph.h). */
#define SB_CPU_1_STATUS                  0x00a01e30UL
#define SB_CPU_2_STATUS                  0x00a01e34UL
#define UREG_UMAC_CURRENT_PC             0x00a05c18UL
#define UREG_LMAC1_CURRENT_PC            0x00a05c1cUL
#define LMPM_CHICK_EXTENDED_ADDR_SPACE   0x00000001UL
#define FW_MEM_EXTENDED_START            0x00040000UL
#define FW_MEM_EXTENDED_END              0x00057fffUL
/* 0: per-image separators (Linux iwl_store_ucode_sec / FreeBSD
 *    iwm_firmware_store_section behaviour, the default).
 * 1: experiment - first CPU1/CPU2 separator of any image splits INIT too. */
#ifndef IWM_INIT_SPLIT_GLOBAL
#define IWM_INIT_SPLIT_GLOBAL 0
#endif

static uint32_t rd(struct iwm_fw_loader *l, uint32_t reg)
{
    return l->adapter->ops->mmio_read32(l->adapter->os_context, reg);
}

static void wr(struct iwm_fw_loader *l, uint32_t reg, uint32_t value)
{
    l->adapter->ops->mmio_write32(l->adapter->os_context, reg, value);
}

static void lock_loader(struct iwm_fw_loader *l)
{
    if (l->adapter->ops->lock)
        l->adapter->ops->lock(l->adapter->os_context);
}

static void unlock_loader(struct iwm_fw_loader *l)
{
    if (l->adapter->ops->unlock)
        l->adapter->ops->unlock(l->adapter->os_context);
}

static int prph_write(struct iwm_fw_loader *l, uint32_t reg, uint32_t value)
{
    int rc;

    rc = iwm_grab_nic_access(l->adapter);
    if (rc != IWM_OK)
        return rc;
    wr(l, HBUS_TARG_PRPH_WADDR, reg | HBUS_PRPH_DWORD);
    wr(l, HBUS_TARG_PRPH_WDAT, value);
    iwm_release_nic_access(l->adapter);
    return IWM_OK;
}

static int prph_read(struct iwm_fw_loader *l, uint32_t reg, uint32_t *value)
{
    int rc;

    if (!value)
        return IWM_EINVAL;
    rc = iwm_grab_nic_access(l->adapter);
    if (rc != IWM_OK)
        return rc;
    wr(l, HBUS_TARG_PRPH_RADDR, reg | HBUS_PRPH_DWORD);
    *value = rd(l, HBUS_TARG_PRPH_RDAT);
    iwm_release_nic_access(l->adapter);
    return IWM_OK;
}

/* Read n dwords of device SRAM (Linux iwl_trans_read_mem: the read address
 * auto-increments). Leaves out[] zero when NIC access is refused. */
static void mem_read(struct iwm_fw_loader *l, uint32_t addr, uint32_t *out,
                     unsigned n)
{
    unsigned i;
    for (i = 0; i < n; ++i)
        out[i] = 0;
    if (!addr || iwm_grab_nic_access(l->adapter) != IWM_OK)
        return;
    wr(l, HBUS_TARG_MEM_RADDR, addr);
    for (i = 0; i < n; ++i)
        out[i] = rd(l, HBUS_TARG_MEM_RDAT);
    iwm_release_nic_access(l->adapter);
}
static int prph_update(struct iwm_fw_loader *l, uint32_t reg,
                       uint32_t set, uint32_t clear)
{
    uint32_t value;
    int rc;

    rc = prph_read(l, reg, &value);
    if (rc != IWM_OK)
        return rc;
    value = (value & ~clear) | set;
    return prph_write(l, reg, value);
}

void iwm_fw_loader_init(struct iwm_fw_loader *l, struct iwm_adapter *a)
{
    if (!l)
        return;
    memset(l, 0, sizeof(*l));
    l->adapter = a;
    l->stage = IWM_FWLS_IDLE;
    l->last_error = IWM_OK;
}

void iwm_fw_loader_bind_rx(struct iwm_fw_loader *l, struct iwm_rx_queue *q)
{
    if (l)
        l->rxq = q;
}

void iwm_fw_loader_bind_tx(struct iwm_fw_loader *l,
                           struct iwm_tx_transport *tx)
{
    if (l)
        l->tx = tx;
}

int iwm_fw_loader_alloc(struct iwm_fw_loader *l)
{
    int rc;

    if (!l || !l->adapter || !l->adapter->ops ||
        !l->adapter->ops->dma_alloc || !l->adapter->ops->dma_free)
        return IWM_EINVAL;
    if (l->bounce.vaddr)
        return IWM_OK;
    rc = l->adapter->ops->dma_alloc(l->adapter->os_context,
                                    IWM_FW_DMA_CHUNK_SIZE,
                                    IWM_FW_DMA_ALIGNMENT, &l->bounce);
    if (rc != IWM_OK)
        return rc;
    rc = iwm_dma32_check(l->bounce.paddr, l->bounce.size,
                         IWM_FW_DMA_ALIGNMENT);
    if (rc != IWM_OK) {
        l->adapter->ops->dma_free(l->adapter->os_context, &l->bounce);
        memset(&l->bounce, 0, sizeof(l->bounce));
        return rc;
    }
    l->stage = IWM_FWLS_ALLOCATED;
    return IWM_OK;
}

void iwm_fw_loader_free(struct iwm_fw_loader *l)
{
    if (!l || !l->adapter || !l->adapter->ops)
        return;
    if (l->bounce.vaddr && l->adapter->ops->dma_free)
        l->adapter->ops->dma_free(l->adapter->os_context, &l->bounce);
    memset(&l->bounce, 0, sizeof(l->bounce));
    l->stage = IWM_FWLS_IDLE;
}

void iwm_fw_loader_note_irq(struct iwm_fw_loader *l, uint32_t cause)
{
    if (!l || !cause || cause == 0xffffffffUL)
        return;
    lock_loader(l);
    l->irq_cause |= cause;
    l->irq_seen |= cause;
    ++l->irq_calls;
    if ((cause & (IWM_CSR_INT_SW_ERR | IWM_CSR_INT_HW_ERR)) && !l->err_at)
        l->err_at = l->irq_calls;
    if ((cause & IWM_CSR_INT_ALIVE) && !l->alive_at)
        l->alive_at = l->irq_calls;
    if (cause & IWM_CSR_INT_ALIVE) {
        l->alive_interrupt = 1;
        l->stage = IWM_FWLS_ALIVE_IRQ;
    }
    unlock_loader(l);
}

static void clear_pending(struct iwm_fw_loader *l, uint32_t mask)
{
    lock_loader(l);
    l->irq_cause &= ~mask;
    unlock_loader(l);
}

static int wait_cause(struct iwm_fw_loader *l, uint32_t mask,
                      uint32_t timeout_us)
{
    uint32_t elapsed, pending, cause;

    for (elapsed = 0; elapsed < timeout_us; elapsed += 100U) {
        lock_loader(l);
        pending = l->irq_cause & mask;
        if (pending)
            l->irq_cause &= ~pending;
        unlock_loader(l);
        if (pending)
            return IWM_OK;

        /* Polling is a safe fallback while the bring-up interrupt path is
         * being validated; the normal path is iwm_fw_loader_note_irq(). */
        cause = rd(l, CSR_INT);
        if (cause != 0 && cause != 0xffffffffUL && (cause & mask)) {
            wr(l, CSR_INT, cause & mask);
            return IWM_OK;
        }
        l->adapter->ops->delay_us(l->adapter->os_context, 100U);
    }
    return IWM_ETIMEDOUT;
}

int iwm_fw_section_cpu(const struct iwm_fw_image *image, uint16_t index,
                       uint8_t image_flag)
{
    uint16_t i, cpu_sep, paging_sep;
    const struct iwm_fw_section *sec;

    if (!image || index >= image->section_count ||
        (image_flag != IWM_FW_SEC_INIT && image_flag != IWM_FW_SEC_RUNTIME))
        return 0;
    sec = &image->section[index];
    if (!(sec->flags & image_flag) || (sec->flags & IWM_FW_SEC_SEPARATOR) ||
        sec->data_length == 0)
        return 0;

    cpu_sep = image->section_count;
    paging_sep = image->section_count;
    for (i = 0; i < image->section_count; ++i) {
        /* Linux iwl_store_ucode_sec() and FreeBSD keep one section list per
         * image type, separators included, so a separator only splits the
         * image whose TLV type it carries.  In the API-46 9560 file every
         * separator is a SEC_RT record: the INIT image has none and Linux
         * loads all nine INIT sections as CPU1. */
        if (!(image->section[i].flags & IWM_FW_SEC_SEPARATOR) ||
            (!IWM_INIT_SPLIT_GLOBAL &&
             !(image->section[i].flags & image_flag)))
            continue;
        if (image->section[i].dst_addr == IWM_UCODE_SECTION_CPU1_CPU2 &&
            cpu_sep == image->section_count)
            cpu_sep = i;
        if (image->section[i].dst_addr == IWM_UCODE_SECTION_PAGING &&
            paging_sep == image->section_count)
            paging_sep = i;
    }
    /* Runtime paging is retained for the later paging command, not copied by
     * the initial service-channel load.  Init sections after that global
     * marker still belong to CPU2 in API-46 images. */
    if (image_flag == IWM_FW_SEC_RUNTIME &&
        paging_sep != image->section_count && index > paging_sep)
        return 0;
    if (cpu_sep == image->section_count)
        return 1;
    return index < cpu_sep ? 1 : 2;
}

/* The FH TX interrupt is a two-level latch: CSR_INT bit 27 is only a summary
 * of CSR_FH_INT_STATUS.  Clearing CSR_INT alone leaves the source bit set, so
 * the summary re-asserts and the next chunk wait returns before its DMA has
 * finished.  Clear the source first, then the summary (iwlwifi/iwm ISR does
 * the same on every FH_TX). */
static void ack_fh_tx(struct iwm_fw_loader *l)
{
    wr(l, CSR_FH_INT_STATUS, FH_INT_TX_MASK);
    wr(l, CSR_INT, IWM_CSR_INT_FH_TX);
}

static int load_chunk(struct iwm_fw_loader *l, uint32_t dst,
                      uint32_t length)
{
    uint32_t hi;
    int rc;

    clear_pending(l, IWM_CSR_INT_FH_TX);
    ack_fh_tx(l); /* discard a stale completion at both latch levels */
    rc = iwm_grab_nic_access(l->adapter);
    if (rc != IWM_OK)
        return rc;
    wr(l, FH_TCSR_CONFIG(FH_SRVC_CHNL), FH_TCSR_PAUSE);
    wr(l, FH_SRVC_SRAM_ADDR(FH_SRVC_CHNL), dst);
    wr(l, FH_TFDIB_CTRL0(FH_SRVC_CHNL), l->bounce.paddr);
    /* struct iwm_dma_block is intentionally DMA32, so address bits 35:32
     * are zero.  CTRL1 places those high bits at 31:28. */
    hi = 0;
    wr(l, FH_TFDIB_CTRL1(FH_SRVC_CHNL), hi | length);
    wr(l, FH_TCSR_BUF_STATUS(FH_SRVC_CHNL),
       (1UL << FH_TCSR_TB_NUM_POS) | (1UL << FH_TCSR_TB_IDX_POS) |
       FH_TCSR_TFDB_VALID);
    wr(l, FH_TCSR_CONFIG(FH_SRVC_CHNL),
       FH_TCSR_ENABLE | FH_TCSR_CREDIT_DISABLE | FH_TCSR_HOST_ENDTFD);
    iwm_release_nic_access(l->adapter);
    rc = wait_cause(l, IWM_CSR_INT_FH_TX, IWM_FW_LOAD_TIMEOUT_US);
    ack_fh_tx(l);
    return rc;
}

static int load_section(struct iwm_fw_loader *l,
                        const struct iwm_fw_section *sec)
{
    uint32_t offset, count, dst;
    int extended, rc;

    for (offset = 0; offset < sec->data_length; offset += count) {
        count = IWM_MIN(l->bounce.size, sec->data_length - offset);
        rc = iwm_blob_read(l->adapter->firmware.store,
                           sec->data_offset + offset,
                           (uint8_t *)l->bounce.vaddr, (uint16_t)count);
        if (rc != IWM_OK)
            return rc;
        dst = sec->dst_addr + offset;
        extended = dst >= FW_MEM_EXTENDED_START && dst <= FW_MEM_EXTENDED_END;
        if (extended) {
            rc = prph_update(l, LMPM_CHICK,
                             LMPM_CHICK_EXTENDED_ADDR_SPACE, 0);
            if (rc != IWM_OK)
                return rc;
        }
        rc = load_chunk(l, dst, count);
        if (extended)
            (void)prph_update(l, LMPM_CHICK, 0,
                              LMPM_CHICK_EXTENDED_ADDR_SPACE);
        if (rc != IWM_OK)
            return rc;
        l->loaded_bytes += count;
        l->last_dst = dst;
    }
    ++l->loaded_sections;
    return IWM_OK;
}

/* FreeBSD iwm_pcie_load_cpu_sections_8000() holds MAC access (iwm_nic_lock)
 * around every FH_UCODE_LOAD_STATUS read and write; Linux uses its
 * *_direct32 accessors at the same points. The register sits in the FH
 * block, which is only reliably accessible while the MAC is awake. */
static int load_status_or(struct iwm_fw_loader *l, uint32_t bits)
{
    int rc = iwm_grab_nic_access(l->adapter);

    if (rc != IWM_OK)
        return rc;
    wr(l, FH_UCODE_LOAD_STATUS, rd(l, FH_UCODE_LOAD_STATUS) | bits);
    iwm_release_nic_access(l->adapter);
    return IWM_OK;
}

static int load_status_write(struct iwm_fw_loader *l, uint32_t value)
{
    int rc = iwm_grab_nic_access(l->adapter);

    if (rc != IWM_OK)
        return rc;
    wr(l, FH_UCODE_LOAD_STATUS, value);
    iwm_release_nic_access(l->adapter);
    return IWM_OK;
}

static int load_cpu(struct iwm_fw_loader *l, uint8_t image_flag, int cpu)
{
    const struct iwm_fw_image *image;
    uint16_t i;
    uint32_t bit;
    int rc, found;

    image = &l->adapter->firmware.catalog;

    bit = 1U;
    found = 0;
    for (i = 0; i < image->section_count; ++i) {
        if (iwm_fw_section_cpu(image, i, image_flag) != cpu)
            continue;
        found = 1;
        rc = load_section(l, &image->section[i]);
        if (rc != IWM_OK)
            return rc;
        rc = load_status_or(l, cpu == 1 ? bit : (bit << 16));
        if (rc != IWM_OK)
            return rc;
        bit = (bit << 1) | 1U;
    }
    if (!found && cpu == 1)
        return IWM_EFORMAT;
    return IWM_OK;
}

/* SB CPU1/CPU2 status, UMAC PC, LMAC PC: shows whether each CPU leaves its
 * entry point after the load-status hand-off. */
static void snap_sb(struct iwm_fw_loader *l, uint32_t out[4])
{
    (void)prph_read(l, SB_CPU_1_STATUS, &out[0]);
    (void)prph_read(l, SB_CPU_2_STATUS, &out[1]);
    (void)prph_read(l, UREG_UMAC_CURRENT_PC, &out[2]);
    (void)prph_read(l, UREG_LMAC1_CURRENT_PC, &out[3]);
}

static int fail_load(struct iwm_fw_loader *l, int rc)
{
    struct iwm_tx_debug txd;

    l->last_csr_int = rd(l, CSR_INT);
    l->last_fh_int = rd(l, CSR_FH_INT_STATUS);
    l->last_load_status = rd(l, FH_UCODE_LOAD_STATUS);
    l->last_gp_cntrl = rd(l, CSR_GP_CNTRL);
    l->last_hw_if = rd(l, CSR_HW_IF_CONFIG_REG);
    (void)prph_read(l, SB_CPU_1_STATUS, &l->last_sb[0]);
    (void)prph_read(l, SB_CPU_2_STATUS, &l->last_sb[1]);
    (void)prph_read(l, UREG_UMAC_CURRENT_PC, &l->last_sb[2]);
    (void)prph_read(l, UREG_LMAC1_CURRENT_PC, &l->last_sb[3]);
    /* Firmware error tables: dword 0 'valid', dword 1 error_id. */
    /* Kept here: iwm_fw_abort() below clears the catalog. */
    l->last_lmac_err_addr = l->adapter->firmware.catalog.lmac_err_addr;
    l->last_umac_err_addr = l->adapter->firmware.catalog.umac_err_addr;
    mem_read(l, l->adapter->firmware.catalog.lmac_err_addr,
             l->last_lmac_err, IWM_FW_ERRTAB_DWORDS);
    mem_read(l, l->adapter->firmware.catalog.umac_err_addr,
             l->last_umac_err, IWM_FW_ERRTAB_DWORDS);
    if (l->rxq) {
        iwm_rx_debug(l->rxq, l->last_rfh);
        /* RIDX moved but nothing closed: did the RFH write a frame into the
         * first buffer anyway (status-block write lost) or nothing at all? */
        if (l->rxq->buffers.vaddr)
            memcpy(l->last_rxbuf, l->rxq->buffers.vaddr,
                   sizeof(l->last_rxbuf));
        if (l->rxq->used_desc.vaddr)
            memcpy(l->last_used, l->rxq->used_desc.vaddr,
                   sizeof(l->last_used));
    }
    if (l->tx) {
        iwm_tx_debug_snapshot(l->tx, &txd);
        l->last_tx[0] = txd.keep_warm;
        l->last_tx[1] = txd.cmd_cbbc;
        l->last_tx[2] = txd.fh_config_0;
        l->last_tx[3] = txd.fh_config_7;
        l->last_tx[4] = txd.fh_chicken;
        l->last_tx[5] = txd.scd_sram_base;
        l->last_tx[6] = txd.scd_dram_base;
        l->last_tx[7] = txd.scd_txfact;
        l->last_tx[8] = txd.scd_cmd_rdptr;
        l->last_tx[9] = txd.scd_cmd_wrptr;
        l->last_tx[10] = txd.scd_cmd_status;
        l->last_tx[11] = txd.hbus_wrptr;
        l->last_tx_state[0] = txd.allocated;
        l->last_tx_state[1] = txd.prepared;
        l->last_tx_state[2] = txd.scheduler_started;
        iwm_tx_stop(l->tx);
    }
    wr(l, CSR_INT_MASK, 0);
    l->last_error = rc;
    l->stage = IWM_FWLS_FAILED;
    l->adapter->state = IWM_ADAPTER_ERROR;
    (void)iwm_fw_abort(&l->adapter->firmware, rc);
    return rc;
}

int iwm_fw_loader_start(struct iwm_fw_loader *l, uint8_t image_flag)
{
    int rc;

    if (!l || !l->adapter || !l->adapter->ops ||
        !l->adapter->ops->mmio_read32 || !l->adapter->ops->mmio_write32 ||
        !l->adapter->ops->delay_us)
        return IWM_EINVAL;
    if (l->adapter->firmware.state != IWM_FW_LOADING ||
        !l->adapter->firmware.store)
        return IWM_ESTATE;
    if (l->adapter->cfg->family < IWM_FAMILY_8000)
        return IWM_ENOTSUP;
    rc = iwm_fw_loader_alloc(l);
    if (rc != IWM_OK)
        return fail_load(l, rc);

    l->loaded_bytes = 0;
    l->loaded_sections = 0;
    l->last_dst = 0;
    l->last_error = IWM_OK;
    l->alive_interrupt = 0;
    l->alive_notification = 0;
    l->irq_cause = 0;
    l->irq_seen = 0;
    l->irq_calls = 0;
    l->alive_at = 0;
    l->err_at = 0;
    memset(l->early_sb, 0, sizeof(l->early_sb));
    l->last_csr_int = 0;
    l->last_fh_int = 0;
    l->last_load_status = 0;
    l->last_gp_cntrl = 0;
    memset(l->last_rfh, 0, sizeof(l->last_rfh));
    memset(l->last_rxbuf, 0, sizeof(l->last_rxbuf));
    memset(l->last_used, 0, sizeof(l->last_used));
    memset(l->last_tx, 0, sizeof(l->last_tx));
    memset(l->last_tx_state, 0, sizeof(l->last_tx_state));

    rc = iwm_prepare_card_hw(l->adapter);
    if (rc != IWM_OK)
        return fail_load(l, rc);
    l->stage = IWM_FWLS_PREPARED;
    {
        /* Linux iwl_pcie_nic_init() -> iwl_mvm_nic_config(), before RX/TX:
         * the Linux iwlwifi trace of this card (HW_REV 0x312) writes
         * CSR_HW_IF_CONFIG_REG = 0x18489002, i.e. the low nibble of
         * CSR_HW_REV unchanged.  Radio type/step/dash come from the
         * firmware PHY_SKU TLV. */
        uint32_t phy = l->adapter->firmware.catalog.phy_config;
        uint32_t val = l->adapter->hw_rev & 0x0fUL;
        val |= (phy & 3UL) << 10;
        val |= ((phy >> 4) & 3UL) << 12;
        val |= ((phy >> 2) & 3UL) << 14;
        wr(l, CSR_HW_IF_CONFIG_REG,
           (rd(l, CSR_HW_IF_CONFIG_REG) & ~CSR_HW_IF_NIC_CONFIG_MASK) | val);
    }

    /* Linux iwl_pcie_nic_init() prepares RX and TX before loading firmware.
     * The pre-ALIVE TX phase publishes keep-warm and command-TFD DMA state;
     * the SCD itself is intentionally started only after a valid ALIVE. */
    if (!l->rxq || !l->tx)
        return fail_load(l, IWM_ESTATE);
    rc = iwm_rx_program(l->rxq);
    if (rc != IWM_OK)
        return fail_load(l, rc);
    rc = iwm_tx_prepare(l->tx);
    if (rc != IWM_OK)
        return fail_load(l, rc);

    wr(l, CSR_INT_MASK, 0);
    wr(l, CSR_INT, 0xffffffffUL);
    wr(l, CSR_UCODE_DRV_GP1_CLR, CSR_UCODE_SW_BIT_RFKILL);
    wr(l, CSR_UCODE_DRV_GP1_CLR, CSR_UCODE_DRV_GP1_CMD_BLOCKED);
    wr(l, CSR_INT, 0xffffffffUL);
    wr(l, CSR_INT_MASK, IWM_CSR_INT_FH_TX);

    /* 8000/9000 secure boot: release the boot CPU, then feed CPU1 and CPU2
     * sections through the FH service channel while updating load status. */
    rc = prph_write(l, WFPM_GP2, WFPM_GP2_VALUE);
    if (rc != IWM_OK)
        return fail_load(l, rc);
    rc = prph_write(l, RELEASE_CPU_RESET, RELEASE_CPU_RESET_BIT);
    if (rc != IWM_OK)
        return fail_load(l, rc);
    l->stage = IWM_FWLS_CPU1;
    rc = load_cpu(l, image_flag, 1);
    if (rc != IWM_OK)
        return fail_load(l, rc);
    wr(l, CSR_INT_MASK, IWM_CSR_INI_SET_MASK);
    rc = load_status_write(l, 0x0000ffffUL);
    if (rc != IWM_OK)
        return fail_load(l, rc);

    if (l->adapter->firmware.catalog.num_cpus > 1) {
        /* No LMPM_SECURE_CPU2_HDR write: Linux and FreeBSD use it only in
         * the pre-8000 loader, never in iwl_pcie_load_given_ucode_8000(). */
        l->stage = IWM_FWLS_CPU2;
        rc = load_cpu(l, image_flag, 2);
        if (rc != IWM_OK)
            return fail_load(l, rc);
        rc = load_status_write(l, 0xffffffffUL);
        if (rc != IWM_OK)
            return fail_load(l, rc);
    }
    snap_sb(l, l->early_sb[0]);
    l->adapter->ops->delay_us(l->adapter->os_context, 10000U);
    snap_sb(l, l->early_sb[1]);
    l->stage = IWM_FWLS_RELEASED;

    /* The CSR ALIVE cause is useful evidence but is not the protocol result.
     * Validate the ALIVE command DMAed into queue 0.  The ISR may acknowledge
     * CSR causes while this bounded task-time loop observes coherent DMA. */
    rc = iwm_rx_wait_alive(l->rxq, IWM_FW_ALIVE_TIMEOUT_US);
    if (rc != IWM_OK)
        return fail_load(l, rc);
    l->alive_notification = 1;
    l->stage = IWM_FWLS_ALIVE_IRQ;
    /* Linux iwl_trans_pcie_fw_alive() starts the scheduler here, after ALIVE
     * makes SCD_SRAM_BASE_ADDR valid and before the first host command. */
    rc = iwm_tx_start_scheduler(l->tx);
    if (rc != IWM_OK)
        return fail_load(l, rc);
    l->last_csr_int = rd(l, CSR_INT);
    l->last_fh_int = rd(l, CSR_FH_INT_STATUS);
    l->last_load_status = rd(l, FH_UCODE_LOAD_STATUS);
    return IWM_OK;
}


