#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "iwm_rx.h"

#define CSR_HW_REV 0x028U
#define CSR_GP_CNTRL 0x024U
#define CSR_GP_CLOCK_READY 0x00000001U
#define HBUS_TARG_PRPH_WADDR 0x444U
#define HBUS_TARG_PRPH_WDAT 0x44cU
#define RFH_RXF_DMA_CFG 0x00a09820U
#define RFH_RXF_RXQ_ACTIVE 0x00a0980cU
#define RFH_Q0_FRBDCB_WIDX_TRG 0x1c80U

struct mock_hw {
    uint32_t regs[2048];
    uint32_t prph_addr;
    uint32_t dma_cfg;
    uint32_t active;
    uint32_t widx;
    uint32_t next_phys;
};

static uint32_t rd(void *c, uint32_t r)
{
    struct mock_hw *h = c;
    if (r == CSR_GP_CNTRL)
        return h->regs[r >> 2] | CSR_GP_CLOCK_READY;
    if ((r >> 2) < IWM_ARRAY_SIZE(h->regs))
        return h->regs[r >> 2];
    return 0;
}

static void wr(void *c, uint32_t r, uint32_t v)
{
    struct mock_hw *h = c;
    if (r == HBUS_TARG_PRPH_WADDR) {
        h->prph_addr = v & 0x00ffffffU;
        return;
    }
    if (r == HBUS_TARG_PRPH_WDAT) {
        if (h->prph_addr == RFH_RXF_DMA_CFG)
            h->dma_cfg = v;
        if (h->prph_addr == RFH_RXF_RXQ_ACTIVE)
            h->active = v;
        return;
    }
    if (r == RFH_Q0_FRBDCB_WIDX_TRG)
        h->widx = v;
    if ((r >> 2) < IWM_ARRAY_SIZE(h->regs))
        h->regs[r >> 2] = v;
}

static void delay(void *c, uint32_t us)
{
    (void)c;
    (void)us;
}

static int dma_alloc(void *c, uint32_t size, uint32_t alignment,
                     struct iwm_dma_block *b)
{
    struct mock_hw *h = c;
    uintptr_t p;
    void *raw = malloc((size_t)size + alignment - 1U);
    if (!raw)
        return IWM_ENOMEM;
    p = ((uintptr_t)raw + alignment - 1U) & ~((uintptr_t)alignment - 1U);
    memset(b, 0, sizeof(*b));
    b->alloc_vaddr = raw;
    b->alloc_size = size + alignment - 1U;
    b->vaddr = (void *)p;
    h->next_phys = IWM_ALIGN_UP(h->next_phys, alignment);
    b->paddr = h->next_phys;
    b->size = size;
    b->alignment = alignment;
    h->next_phys += IWM_ALIGN_UP(size, alignment);
    memset(b->vaddr, 0, size);
    return IWM_OK;
}

static void dma_free(void *c, struct iwm_dma_block *b)
{
    (void)c;
    free(b->alloc_vaddr);
    memset(b, 0, sizeof(*b));
}

static const struct iwm_os_ops ops = {
    rd, wr, delay, dma_alloc, dma_free, NULL, NULL, NULL
};

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void make_alive(uint8_t *p)
{
    memset(p, 0, IWM_RX_BUFFER_SIZE);
    put32(p, 16U);              /* 4-byte command header + 12-byte payload */
    p[4] = IWM_RX_CMD_ALIVE;
    p[5] = 0;                  /* legacy command group */
    put16(p + 8, IWM_RX_ALIVE_STATUS_OK);
    put16(p + 10, 1U);
    put32(p + 12, 0x11223344U);
    put32(p + 16, 0x55667788U);
}

int main(void)
{
    struct mock_hw hw;
    struct iwm_adapter a;
    struct iwm_rx_queue q;
    struct iwm_rx_status *status;
    uint32_t *used;
    uint8_t *buf;

    memset(&hw, 0, sizeof(hw));
    hw.next_phys = 0x10000000U;
    hw.regs[CSR_HW_REV >> 2] = 0x312U;
    iwm_adapter_init(&a, &ops, &hw);
    assert(iwm_adapter_probe(&a, 0x8086, 0xa370, 0, 54) == IWM_OK);
    iwm_rx_init(&q, &a);
    assert(iwm_rx_alloc(&q) == IWM_OK);
    assert(q.free_desc.size == IWM_RX_QUEUE_SIZE * 8U);
    assert(q.used_desc.size == IWM_RX_QUEUE_SIZE * 4U);
    assert(q.buffers.size == IWM_RX_QUEUE_SIZE * IWM_RX_BUFFER_SIZE);
    assert(iwm_rx_program(&q) == IWM_OK);
    assert(q.programmed && q.write_actual == 504U);
    assert(hw.dma_cfg & 0x80000000U);
    assert(hw.active == 0x00010001U);
    assert(hw.widx == 504U);

    buf = (uint8_t *)q.buffers.vaddr + 42U * IWM_RX_BUFFER_SIZE;
    make_alive(buf);
    used = (uint32_t *)q.used_desc.vaddr;
    status = (struct iwm_rx_status *)q.status.vaddr;
    used[0] = 42U;
    status->closed_rb_num = 1U;
    assert(iwm_rx_drain(&q) == IWM_OK);
    assert(q.alive_seen && q.alive.valid);
    assert(q.alive.status == IWM_RX_ALIVE_STATUS_OK);
    assert(q.alive.flags == 1U);
    assert(q.alive.lmac_major == 0x11223344U);
    assert(q.alive.lmac_minor == 0x55667788U);
    assert(q.read == 1U && q.posted == 0U && hw.widx == 0U);

    memset(buf, 0, IWM_RX_BUFFER_SIZE);
    put32(buf, IWM_RX_BUFFER_SIZE);
    assert(iwm_rx_process_buffer(&q, buf, IWM_RX_BUFFER_SIZE) == IWM_EFORMAT);
    assert(q.malformed == 1U);

    iwm_rx_free(&q);
    puts("rx queue tests passed");
    return 0;
}
