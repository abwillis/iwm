/* SPDX-License-Identifier: GPL-2.0-only */
/* Intel 8000/9000 secure firmware section loader.  The register sequence is
 * the FH service-channel flow used by iwlwifi/FreeBSD iwm; OS ownership,
 * allocation and interrupt delivery remain behind struct iwm_os_ops. */
#if !defined(IWM_RING0)
#include <string.h>
#endif
#include "iwm_fw_loader.h"
#include "iwm_rx.h"

#define CSR_INT                         0x008UL
#define CSR_INT_MASK                    0x00cUL
#define CSR_FH_INT_STATUS               0x010UL
#define CSR_UCODE_DRV_GP1_CLR           0x05cUL
#define CSR_UCODE_SW_BIT_RFKILL         0x00000002UL
#define CSR_UCODE_DRV_GP1_CMD_BLOCKED   0x00000004UL
#define HBUS_TARG_PRPH_WADDR            0x444UL
#define HBUS_TARG_PRPH_RADDR            0x448UL
#define HBUS_TARG_PRPH_WDAT             0x44cUL
#define HBUS_TARG_PRPH_RDAT             0x450UL
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
#define LMPM_SECURE_CPU2_HDR_ADDR         0x001e7cUL
#define LMPM_SECURE_CPU2_HDR_MEM_SPACE    0x00420400UL
#define LMPM_CHICK                       0x00a01ff8UL
#define LMPM_CHICK_EXTENDED_ADDR_SPACE   0x00000001UL
#define FW_MEM_EXTENDED_START            0x00040000UL
#define FW_MEM_EXTENDED_END              0x00057fffUL

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
        if (!(image->section[i].flags & IWM_FW_SEC_SEPARATOR))
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

static int load_chunk(struct iwm_fw_loader *l, uint32_t dst,
                      uint32_t length)
{
    uint32_t hi;
    int rc;

    clear_pending(l, IWM_CSR_INT_FH_TX);
    wr(l, CSR_INT, IWM_CSR_INT_FH_TX); /* discard a stale W1C completion */
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
    return wait_cause(l, IWM_CSR_INT_FH_TX, IWM_FW_LOAD_TIMEOUT_US);
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

static int load_cpu(struct iwm_fw_loader *l, uint8_t image_flag, int cpu)
{
    const struct iwm_fw_image *image;
    uint16_t i;
    uint32_t status, bit;
    int rc, found;

    image = &l->adapter->firmware.catalog;
    status = rd(l, FH_UCODE_LOAD_STATUS);
    bit = 1U;
    found = 0;
    for (i = 0; i < image->section_count; ++i) {
        if (iwm_fw_section_cpu(image, i, image_flag) != cpu)
            continue;
        found = 1;
        rc = load_section(l, &image->section[i]);
        if (rc != IWM_OK)
            return rc;
        if (cpu == 1)
            status |= bit;
        else
            status |= bit << 16;
        wr(l, FH_UCODE_LOAD_STATUS, status);
        bit = (bit << 1) | 1U;
    }
    if (!found && cpu == 1)
        return IWM_EFORMAT;
    return IWM_OK;
}

static int fail_load(struct iwm_fw_loader *l, int rc)
{
    l->last_csr_int = rd(l, CSR_INT);
    l->last_fh_int = rd(l, CSR_FH_INT_STATUS);
    l->last_load_status = rd(l, FH_UCODE_LOAD_STATUS);
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
    l->last_csr_int = 0;
    l->last_fh_int = 0;
    l->last_load_status = 0;

    rc = iwm_prepare_card_hw(l->adapter);
    if (rc != IWM_OK)
        return fail_load(l, rc);
    l->stage = IWM_FWLS_PREPARED;

    /* The 9000-family firmware delivers ALIVE through RFH queue 0.  Program
     * the queue before either secure CPU is released; otherwise firmware has
     * nowhere to DMA the notification and boot appears to time out. */
    if (!l->rxq)
        return fail_load(l, IWM_ESTATE);
    rc = iwm_rx_program(l->rxq);
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
    rc = prph_write(l, RELEASE_CPU_RESET, RELEASE_CPU_RESET_BIT);
    if (rc != IWM_OK)
        return fail_load(l, rc);
    l->stage = IWM_FWLS_CPU1;
    rc = load_cpu(l, image_flag, 1);
    if (rc != IWM_OK)
        return fail_load(l, rc);
    wr(l, CSR_INT_MASK, IWM_CSR_INI_SET_MASK);
    wr(l, FH_UCODE_LOAD_STATUS, 0x0000ffffUL);

    if (l->adapter->firmware.catalog.num_cpus > 1) {
        rc = prph_write(l, LMPM_SECURE_CPU2_HDR_ADDR,
                        LMPM_SECURE_CPU2_HDR_MEM_SPACE);
        if (rc != IWM_OK)
            return fail_load(l, rc);
        l->stage = IWM_FWLS_CPU2;
        rc = load_cpu(l, image_flag, 2);
        if (rc != IWM_OK)
            return fail_load(l, rc);
        wr(l, FH_UCODE_LOAD_STATUS, 0xffffffffUL);
    }
    l->stage = IWM_FWLS_RELEASED;

    /* The CSR ALIVE cause is useful evidence but is not the protocol result.
     * Validate the ALIVE command DMAed into queue 0.  The ISR may acknowledge
     * CSR causes while this bounded task-time loop observes coherent DMA. */
    rc = iwm_rx_wait_alive(l->rxq, IWM_FW_ALIVE_TIMEOUT_US);
    if (rc != IWM_OK)
        return fail_load(l, rc);
    l->alive_notification = 1;
    l->stage = IWM_FWLS_ALIVE_IRQ;
    l->last_csr_int = rd(l, CSR_INT);
    l->last_fh_int = rd(l, CSR_FH_INT_STATUS);
    l->last_load_status = rd(l, FH_UCODE_LOAD_STATUS);
    return IWM_OK;
}


