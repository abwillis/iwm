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
#define HBUS_TARG_PRPH_RADDR 0x448U
#define HBUS_TARG_PRPH_RDAT 0x450U
#define RFH_Q0_FRBDCB_WIDX 0x00a08080U
#define CSR_MAC_SHADOW_REG_CTRL 0x0a8U

struct mock_hw {
    uint32_t regs[2048];
    uint32_t prph_addr;
    uint32_t prph_raddr;
    int shadow_mode;    /* 0: trigger always works (old mock), 1: real-HW model */
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
    if (r == HBUS_TARG_PRPH_RDAT && h->prph_raddr == RFH_Q0_FRBDCB_WIDX)
        return h->widx;
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
    if (r == HBUS_TARG_PRPH_RADDR) {
        h->prph_raddr = v & 0x00ffffffU;
        return;
    }
    if (r == HBUS_TARG_PRPH_WDAT) {
        if (h->prph_addr == RFH_Q0_FRBDCB_WIDX)
            h->widx = v;
        if (h->prph_addr == RFH_RXF_DMA_CFG)
            h->dma_cfg = v;
        if (h->prph_addr == RFH_RXF_RXQ_ACTIVE)
            h->active = v;
        return;
    }
    /* Real hardware drops the CSR shadow trigger unless shadow registers
     * are enabled in CSR_MAC_SHADOW_REG_CTRL; the old mock accepted it
     * unconditionally, which is why this test passed while the card saw
     * WIDX 0 (log: "dbg trg 00000000"). */
    if (r == RFH_Q0_FRBDCB_WIDX_TRG) {
        if (!h->shadow_mode ||
            (h->regs[CSR_MAC_SHADOW_REG_CTRL >> 2] & 0x800FFFFFUL) ==
                0x800FFFFFUL)
            h->widx = v;
        return;
    }
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

/* iwm_rx_drain() called again from inside a notification callback: the
 * association layer sends firmware commands and waits for their responses
 * from its RX callback. */
static struct iwm_rx_queue *g_nq;
static unsigned g_nseen[16], g_ncalls;
static int g_nest_at, g_nest_done;

static void nest_cb(void *ctx, uint8_t cmd, uint8_t group, uint16_t seq,
                    const uint8_t *payload, uint32_t len)
{
    unsigned id = payload[0];
    (void)ctx; (void)cmd; (void)group; (void)seq; (void)len;
    ++g_ncalls;
    ++g_nseen[id & 15U];
    if ((int)id == g_nest_at && !g_nest_done) {
        g_nest_done = 1;
        (void)iwm_rx_drain(g_nq);
    }
}

/* The device completes buffer slot: one notification whose payload byte 0 is id. */
static void nest_push(struct iwm_rx_queue *q, unsigned slot, unsigned id)
{
    uint8_t *b = (uint8_t *)q->buffers.vaddr + slot * IWM_RX_BUFFER_SIZE;
    memset(b, 0, 16);
    put32(b, 6U);
    b[4] = 0x77;
    put16(b + 6, 0x8000);
    b[8] = (uint8_t)id;
    ((uint32_t *)q->used_desc.vaddr)[slot] = slot + 1U;
    ((struct iwm_rx_status *)q->status.vaddr)->closed_rb_num = (uint16_t)(slot + 1U);
}

int main(void)
{
    struct mock_hw hw;
    struct iwm_adapter a;
    struct iwm_rx_queue q;
    struct iwm_rx_status *status;
    uint32_t *used;
    uint8_t *buf;
    unsigned i;

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
    assert(q.widx_readback == 504U);
    /* Buffer IDs start at 1: slot 0 carries buffer 0 as ID 1. */
    assert((((uint32_t *)q.free_desc.vaddr)[0] & 0xfffU) == 1U);
    assert((((uint32_t *)q.free_desc.vaddr)[0] & ~0xfffU) == q.buffers.paddr);
    buf = (uint8_t *)q.buffers.vaddr + 42U * IWM_RX_BUFFER_SIZE;
    make_alive(buf);
    used = (uint32_t *)q.used_desc.vaddr;
    status = (struct iwm_rx_status *)q.status.vaddr;
    used[0] = 43U;              /* ID 43 = buffer 42 */
    status->closed_rb_num = 1U;
    assert(iwm_rx_drain(&q) == IWM_OK);
    assert(q.alive_seen && q.alive.valid);
    assert(q.alive.status == IWM_RX_ALIVE_STATUS_OK);
    assert(q.alive.flags == 1U);
    assert(q.alive.lmac_major == 0x11223344U);
    assert(q.alive.lmac_minor == 0x55667788U);
    assert(q.read == 1U && q.posted == 0U && hw.widx == 0U);

    /* A response matched by sequence; a notification is counted, not taken. */
    q.resp_wait = 1;
    q.resp_seen = 0;
    q.resp_seq = 0x0000;
    memset(buf, 0, IWM_RX_BUFFER_SIZE);
    put32(buf, 4U + 12U);
    buf[4] = 0x88;
    put16(buf + 6, 0x8000);        /* SEQ_RX_FRAME: unsolicited */
    assert(iwm_rx_process_buffer(&q, buf, IWM_RX_BUFFER_SIZE) == IWM_OK);
    assert(!q.resp_seen && q.notifications == 1U);
    put16(buf + 6, 0x0000);
    put16(buf + 8, 0);             /* offset */
    put16(buf + 10, 0x0800);       /* length */
    put16(buf + 12, 0);            /* type */
    put16(buf + 14, 0);            /* status */
    assert(iwm_rx_process_buffer(&q, buf, IWM_RX_BUFFER_SIZE) == IWM_OK);
    assert(q.resp_seen && q.resp_cmd == 0x88 && q.resp_len == 12U);
    assert(q.resp[2] == 0x00 && q.resp[3] == 0x08);
    q.resp_wait = 0;
    memset(buf, 0, IWM_RX_BUFFER_SIZE);
    put32(buf, IWM_RX_BUFFER_SIZE);
    assert(iwm_rx_process_buffer(&q, buf, IWM_RX_BUFFER_SIZE) == IWM_EFORMAT);
    assert(q.malformed == 1U);
    /* ID 0 is invalid: counted, skipped, nothing recycled. */
    used[1] = 0U;
    status->closed_rb_num = 2U;
    assert(iwm_rx_drain(&q) == IWM_EFORMAT);
    assert(q.malformed == 2U && q.read == 2U && q.posted == 0U);
    iwm_rx_free(&q);

    /* Same sequence against the stricter hardware model: the free-buffer
     * write index must be visible to the RFH after programming. */
    memset(&hw, 0, sizeof(hw));
    hw.shadow_mode = 1;
    hw.next_phys = 0x10000000U;
    hw.regs[CSR_HW_REV >> 2] = 0x312U;
    iwm_adapter_init(&a, &ops, &hw);
    assert(iwm_adapter_probe(&a, 0x8086, 0xa370, 0, 54) == IWM_OK);
    iwm_rx_init(&q, &a);
    assert(iwm_rx_alloc(&q) == IWM_OK);
    assert(iwm_rx_program(&q) == IWM_OK);
    assert(hw.widx == 504U);
    assert(q.widx_readback == 504U);
    iwm_rx_free(&q);
    /* Six completed buffers; the callback of the first one drains again.  Each
     * buffer must be delivered exactly once (the old loop made 13 callbacks
     * and posted 13 buffers back, d27 log: stale and duplicate frames). */
    memset(&hw, 0, sizeof(hw));
    hw.next_phys = 0x10000000U;
    hw.regs[CSR_HW_REV >> 2] = 0x312U;
    iwm_adapter_init(&a, &ops, &hw);
    assert(iwm_adapter_probe(&a, 0x8086, 0xa370, 0, 54) == IWM_OK);
    iwm_rx_init(&q, &a);
    assert(iwm_rx_alloc(&q) == IWM_OK && iwm_rx_program(&q) == IWM_OK);
    q.notif_cb = nest_cb;
    g_nq = &q;
    g_nest_at = 1;
    for (i = 0; i < 6U; ++i)
        nest_push(&q, i, i + 1U);
    (void)iwm_rx_drain(&q);
    for (i = 1; i <= 6U; ++i)
        assert(g_nseen[i] == 1U);
    assert(g_ncalls == 6U && q.nested_drains == 1U && q.draining == 0U);
    assert(q.read == 6U && q.posted == 5U);
    iwm_rx_free(&q);
    puts("rx queue tests passed");
    return 0;
}
