/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "iwm_fw_loader.h"
#include "iwm_rx.h"

#define CSR_INT 0x008U
#define CSR_GP_CNTRL 0x024U
#define CSR_HW_REV 0x028U
#define CSR_GP_CLOCK_READY 0x1U
#define CSR_GP_INIT_DONE 0x4U
#define FH_TCSR_CONFIG_9 (0x1d00U + 0x20U * 9U)
#define FH_TCSR_ENABLE 0x80000000UL
#define FH_UCODE_LOAD_STATUS 0x1af0U
#define HBUS_TARG_PRPH_WADDR 0x444U
#define HBUS_TARG_PRPH_RADDR 0x448U
#define HBUS_TARG_PRPH_WDAT 0x44cU
#define HBUS_TARG_PRPH_RDAT 0x450U

struct mock_hw {
    uint32_t regs[2048];
    unsigned transfers;
    int signal_alive;
    uint32_t prph_waddr;
    uint32_t prph_raddr;
    uint32_t load_status;
    uint32_t next_phys;
    struct iwm_rx_queue *rxq;
};

struct mem_store {
    uint8_t *buf;
    uint32_t len;
};

static int store_resize(void *p, uint32_t n)
{
    struct mem_store *m = (struct mem_store *)p;
    uint8_t *b = (uint8_t *)realloc(m->buf, n ? n : 1U);
    if (!b)
        return IWM_ENOMEM;
    m->buf = b;
    m->len = n;
    memset(m->buf, 0, n ? n : 1U);
    return IWM_OK;
}

static int store_write(void *p, uint32_t off, const uint8_t *d, uint16_t n)
{
    struct mem_store *m = (struct mem_store *)p;
    if (off > m->len || (uint32_t)n > m->len - off)
        return IWM_EOVERFLOW;
    memcpy(m->buf + off, d, n);
    return IWM_OK;
}

static int store_read(void *p, uint32_t off, uint8_t *d, uint16_t n)
{
    struct mem_store *m = (struct mem_store *)p;
    if (off > m->len || (uint32_t)n > m->len - off)
        return IWM_EOVERFLOW;
    memcpy(d, m->buf + off, n);
    return IWM_OK;
}

static void store_reset(void *p)
{
    struct mem_store *m = (struct mem_store *)p;
    free(m->buf);
    m->buf = NULL;
    m->len = 0;
}

static const struct iwm_blob_ops store_ops = {
    store_resize, store_write, store_read, store_reset
};

static uint32_t mock_read(void *p, uint32_t off)
{
    struct mock_hw *m = (struct mock_hw *)p;
    if (off == FH_UCODE_LOAD_STATUS)
        return m->load_status;
    if (off == HBUS_TARG_PRPH_RDAT && m->prph_raddr == FH_UCODE_LOAD_STATUS)
        return m->load_status;
    return m->regs[(off >> 2) & 2047U];
}

static void inject_alive(struct mock_hw *m)
{
    uint8_t *b;
    uint32_t *used;
    struct iwm_rx_status *status;

    if (!m->rxq || !m->rxq->programmed)
        return;
    b = (uint8_t *)m->rxq->buffers.vaddr;
    used = (uint32_t *)m->rxq->used_desc.vaddr;
    status = (struct iwm_rx_status *)m->rxq->status.vaddr;
    memset(b, 0, IWM_RX_BUFFER_SIZE);
    b[0] = 16;                 /* frame length: header 4 + payload 12 */
    b[4] = IWM_RX_CMD_ALIVE;
    b[8] = (uint8_t)IWM_RX_ALIVE_STATUS_OK;
    b[9] = (uint8_t)(IWM_RX_ALIVE_STATUS_OK >> 8);
    b[12] = 0x46;              /* deterministic version diagnostics */
    used[0] = 0;
    status->closed_rb_num = 1;
}

static void mock_write(void *p, uint32_t off, uint32_t value)
{
    struct mock_hw *m = (struct mock_hw *)p;
    unsigned i = (off >> 2) & 2047U;

    if (off == CSR_INT) {
        m->regs[i] &= ~value; /* write-one-to-clear */
        return;
    }
    if (off == HBUS_TARG_PRPH_WADDR) {
        m->prph_waddr = value & 0x00ffffffU;
        return;
    }
    if (off == HBUS_TARG_PRPH_RADDR) {
        m->prph_raddr = value & 0x00ffffffU;
        return;
    }
    if (off == HBUS_TARG_PRPH_WDAT) {
        if (m->prph_waddr == FH_TCSR_CONFIG_9 &&
            (value & FH_TCSR_ENABLE)) {
            ++m->transfers;
            m->regs[CSR_INT >> 2] |= IWM_CSR_INT_FH_TX;
        }
        if (m->prph_waddr == FH_UCODE_LOAD_STATUS) {
            m->load_status = value;
            if (value == 0xffffffffUL && m->signal_alive) {
                m->regs[CSR_INT >> 2] |= IWM_CSR_INT_ALIVE;
                inject_alive(m);
            }
        }
        return;
    }
    if (off == FH_UCODE_LOAD_STATUS) {
        m->load_status = value;
        if (value == 0xffffffffUL && m->signal_alive) {
            m->regs[CSR_INT >> 2] |= IWM_CSR_INT_ALIVE;
            inject_alive(m);
        }
        return;
    }
    if (off == FH_TCSR_CONFIG_9 && (value & FH_TCSR_ENABLE)) {
        ++m->transfers;
        m->regs[CSR_INT >> 2] |= IWM_CSR_INT_FH_TX;
    }
    m->regs[i] = value;
    if (off == CSR_GP_CNTRL && (value & CSR_GP_INIT_DONE))
        m->regs[i] |= CSR_GP_CLOCK_READY;
}

static void mock_delay(void *p, uint32_t usec)
{
    (void)p;
    (void)usec;
}

static int mock_dma_alloc(void *p, uint32_t size, uint32_t alignment,
                          struct iwm_dma_block *b)
{
    struct mock_hw *m = (struct mock_hw *)p;
    size_t raw_addr, aligned;
    void *raw;

    raw = malloc(size + alignment - 1U);
    if (!raw)
        return IWM_ENOMEM;
    raw_addr = (size_t)raw;
    aligned = (raw_addr + alignment - 1U) & ~((size_t)alignment - 1U);
    memset(b, 0, sizeof(*b));
    b->alloc_vaddr = raw;
    b->alloc_size = size + alignment - 1U;
    b->vaddr = (void *)aligned;
    m->next_phys = IWM_ALIGN_UP(m->next_phys, alignment);
    b->paddr = m->next_phys;
    b->size = size;
    b->alignment = alignment;
    m->next_phys += IWM_ALIGN_UP(size, alignment);
    memset(b->vaddr, 0, size);
    return IWM_OK;
}

static void mock_dma_free(void *p, struct iwm_dma_block *b)
{
    (void)p;
    free(b->alloc_vaddr ? b->alloc_vaddr : b->vaddr);
    memset(b, 0, sizeof(*b));
}

int main(void)
{
    struct mock_hw hw;
    struct iwm_os_ops ops;
    struct iwm_adapter a;
    struct mem_store ms;
    struct iwm_blob blob;
    struct iwm_fw_loader loader;
    struct iwm_rx_queue rxq;
    struct iwm_fw_image *im;
    uint8_t bytes[64];
    unsigned i;
    int rc;

    memset(&hw, 0, sizeof(hw));
    memset(&ops, 0, sizeof(ops));
    memset(&ms, 0, sizeof(ms));
    memset(bytes, 0x5a, sizeof(bytes));
    hw.signal_alive = 1;
    hw.next_phys = 0x00100000UL;
    ops.mmio_read32 = mock_read;
    ops.mmio_write32 = mock_write;
    ops.delay_us = mock_delay;
    ops.dma_alloc = mock_dma_alloc;
    ops.dma_free = mock_dma_free;
    hw.regs[CSR_HW_REV >> 2] = 0x312;

    iwm_adapter_init(&a, &ops, &hw);
    assert(iwm_adapter_probe(&a, 0x8086, 0xa370, 0, 54) == IWM_OK);
    iwm_blob_bind(&blob, &store_ops, &ms);
    assert(iwm_blob_resize(&blob, sizeof(bytes)) == IWM_OK);
    assert(iwm_blob_write(&blob, 0, bytes, sizeof(bytes)) == IWM_OK);
    assert(iwm_adapter_set_fw_store(&a, &blob) == IWM_OK);

    im = &a.firmware.catalog;
    memset(im, 0, sizeof(*im));
    im->num_cpus = 2;
    im->section_count = 7;
    im->runtime_count = 3;
    im->init_count = 2;
    for (i = 0; i < im->section_count; ++i) {
        im->section[i].data_offset = i * 8U;
        im->section[i].data_length = 8;
        im->section[i].dst_addr = 0x1000U + i * 0x100U;
    }
    im->section[0].flags = IWM_FW_SEC_RUNTIME;
    im->section[1].flags = IWM_FW_SEC_INIT;
    im->section[2].flags = IWM_FW_SEC_RUNTIME | IWM_FW_SEC_SEPARATOR;
    im->section[2].dst_addr = IWM_UCODE_SECTION_CPU1_CPU2;
    im->section[2].data_length = 0;
    im->section[3].flags = IWM_FW_SEC_RUNTIME;
    im->section[4].flags = IWM_FW_SEC_RUNTIME | IWM_FW_SEC_SEPARATOR;
    im->section[4].dst_addr = IWM_UCODE_SECTION_PAGING;
    im->section[4].data_length = 0;
    im->section[5].flags = IWM_FW_SEC_RUNTIME;
    im->section[6].flags = IWM_FW_SEC_INIT;

    assert(iwm_fw_section_cpu(im, 1, IWM_FW_SEC_INIT) == 1);
    assert(iwm_fw_section_cpu(im, 6, IWM_FW_SEC_INIT) == 2);
    assert(iwm_fw_section_cpu(im, 0, IWM_FW_SEC_RUNTIME) == 1);
    assert(iwm_fw_section_cpu(im, 3, IWM_FW_SEC_RUNTIME) == 2);
    assert(iwm_fw_section_cpu(im, 5, IWM_FW_SEC_RUNTIME) == 0);

    a.firmware.state = IWM_FW_LOADING;
    a.state = IWM_ADAPTER_FW_WAIT;
    iwm_rx_init(&rxq, &a);
    assert(iwm_rx_alloc(&rxq) == IWM_OK);
    hw.rxq = &rxq;
    iwm_fw_loader_init(&loader, &a);
    iwm_fw_loader_bind_rx(&loader, &rxq);
    rc = iwm_fw_loader_start(&loader, IWM_FW_SEC_INIT);
    if (rc != IWM_OK)
        fprintf(stderr, "loader rc %d stage %u loaded %u transfers %u rx packets %lu bad %lu closed %u\n",
                rc, (unsigned)loader.stage, (unsigned)loader.loaded_sections,
                hw.transfers, (unsigned long)rxq.packets,
                (unsigned long)rxq.malformed, (unsigned)rxq.last_closed);
    assert(rc == IWM_OK);
    assert(loader.loaded_sections == 2);
    assert(loader.loaded_bytes == 16);
    assert(loader.alive_interrupt == 0);
    assert(loader.alive_notification == 1);
    assert(loader.stage == IWM_FWLS_ALIVE_IRQ);
    assert(a.firmware.state == IWM_FW_LOADING); /* RX ALIVE still required */
    assert(hw.transfers == 2);

    iwm_fw_loader_free(&loader);

    /* A missing early ALIVE is a load failure, not a successful COMMIT. */
    hw.signal_alive = 0;
    hw.regs[CSR_INT >> 2] = 0;
    hw.load_status = 0;
    a.firmware.state = IWM_FW_LOADING;
    a.state = IWM_ADAPTER_FW_WAIT;
    iwm_fw_loader_init(&loader, &a);
    iwm_fw_loader_bind_rx(&loader, &rxq);
    assert(iwm_fw_loader_start(&loader, IWM_FW_SEC_INIT) == IWM_ETIMEDOUT);
    assert(loader.stage == IWM_FWLS_FAILED);
    assert(loader.last_error == IWM_ETIMEDOUT);
    assert(loader.alive_interrupt == 0);
    assert(loader.alive_notification == 0);
    assert(a.firmware.state == IWM_FW_FAILED);
    iwm_fw_loader_free(&loader);
    iwm_rx_free(&rxq);
    iwm_blob_reset(&blob);
    puts("firmware loader tests passed");
    return 0;
}


