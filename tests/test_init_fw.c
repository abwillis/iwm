/* SPDX-License-Identifier: GPL-2.0-only */
/* INIT command sequence against a scripted firmware model. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "iwm_init_fw.h"

#define CSR_GP_CNTRL         0x024U
#define CSR_HW_REV           0x028U
#define CSR_RESET            0x020U
#define HBUS_TARG_WRPTR      0x460U

struct mock_hw {
    uint32_t regs[2048];
    uint32_t next_phys;
    struct iwm_rx_queue *rxq;
    struct iwm_tx_transport *tx;
    uint16_t rbid;
    uint16_t closed;
    unsigned cmds;
    uint8_t order[64];
    int drop_init_complete;
};

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void push(struct mock_hw *m, uint8_t cmd, uint16_t seq,
                 const uint8_t *pl, uint32_t len)
{
    uint8_t *b = (uint8_t *)m->rxq->buffers.vaddr +
                 (uint32_t)m->rbid * IWM_RX_BUFFER_SIZE;
    uint32_t *used = (uint32_t *)m->rxq->used_desc.vaddr;
    struct iwm_rx_status *s = (struct iwm_rx_status *)m->rxq->status.vaddr;
    memset(b, 0, 16);
    put32(b, 4U + len);
    b[4] = cmd;
    /* Firmware may tag a legacy notification with LONG_GROUP (1). */
    b[5] = (uint8_t)((cmd == 0x6b && (seq & 1U)) ? 1U : 0U);
    put16(b + 6, seq);
    if (len)
        memcpy(b + 8, pl, len);
    used[m->closed] = (uint32_t)m->rbid + 1U;
    m->rbid = (uint16_t)((m->rbid + 1U) % 500U);
    m->closed = (uint16_t)((m->closed + 1U) & IWM_RX_QUEUE_MASK);
    s->closed_rb_num = m->closed;
}

/* Firmware: answer the command in queue-0 slot (wrptr - 1). */
static void fw_cmd(struct mock_hw *m, uint16_t idx)
{
    const uint8_t *c = (const uint8_t *)m->tx->cmd_buffers.vaddr +
                       (uint32_t)(idx & (IWM_TX_CMD_WINDOW - 1U)) *
                       IWM_TX_CMD_BUFFER_SIZE;
    uint8_t cmd = c[0];
    uint16_t seq = (uint16_t)(c[2] | (c[3] << 8));
    static uint8_t r[8 + 3084];
    memset(r, 0, sizeof(r));
    if (m->cmds < sizeof(m->order))
        m->order[m->cmds] = cmd;
    ++m->cmds;
    if (cmd == 0x88) {
        uint16_t type = (uint16_t)(c[6] | (c[7] << 8));
        uint16_t off = (uint16_t)(c[8] | (c[9] << 8));
        uint16_t n = 0, st = 0;
        if (type == 1)
            n = 100;
        else if (type == 3)
            n = off == 0 ? 2048 : 300;
        else
            st = 0x0100;
        put16(r, off); put16(r + 2, n ? n : 0x800); put16(r + 4, type);
        put16(r + 6, st);
        memset(r + 8, (int)(0x30 + type), n);
        push(m, cmd, seq, r, 8U + n);
        return;
    }
    if (cmd == 0x6a) {
        assert(c[4] == 0x18 && c[6] == 0x33);          /* phy_cfg 0x330018 */
        assert(c[8] == 0xdb && c[11] == 0x1f);         /* flow 0x1f8e51db */
        push(m, cmd, seq, r, 4);
        /* PHY-DB results, an unrelated notification, then INIT complete */
        /* iwl_calib_res_notif_phy_db: le16 type, le16 length, data */
        put16(r, 1); put16(r + 2, 6); memset(r + 4, 0xab, 6);
        push(m, 0x6b, 0x8000, r, 10);
        put16(r, 4); put16(r + 2, 10); memset(r + 4, 0xcd, 10);
        push(m, 0x6b, 0x8001, r, 14);
        /* TXP channel group as seen on the 9560: 3084-byte payload */
        put16(r, 5); put16(r + 2, 3080); memset(r + 4, 0xef, 3080);
        push(m, 0x6b, 0x8000, r, 3084);
        /* Length field larger than the payload: refused */
        put16(r, 2); put16(r + 2, 9);
        push(m, 0x6b, 0x8000, r, 8);
        push(m, 0x77, 0x8002, r, 4);
        if (!m->drop_init_complete)
            push(m, 0x04, 0x8003, r, 4);
        return;
    }
    if (cmd == 0xc8) {
        assert(c[4] == 0x5a && c[5] == 0x5a && c[6] == 0x10);
        put32(r, 0); put16(r + 4, 0x5553);
        push(m, cmd, seq, r, 20);
        return;
    }
    if (cmd == 0x98)
        assert(c[4] == 3);
    push(m, cmd, seq, r, 4);
}

static uint32_t mrd(void *p, uint32_t off)
{
    struct mock_hw *m = (struct mock_hw *)p;
    if (off == CSR_GP_CNTRL)
        return m->regs[off >> 2] | 1U;
    if (off == CSR_RESET)
        return m->regs[off >> 2] | 0x100U;
    return m->regs[(off >> 2) & 2047U];
}

static void mwr(void *p, uint32_t off, uint32_t v)
{
    struct mock_hw *m = (struct mock_hw *)p;
    m->regs[(off >> 2) & 2047U] = v;
    if (off == HBUS_TARG_WRPTR && m->tx && m->tx->scheduler_started &&
        (v & 0xffU) != 0)
        fw_cmd(m, (uint16_t)((v & 0xffU) - 1U));
}

static void mdl(void *p, uint32_t u) { (void)p; (void)u; }

static int malloc_dma(void *p, uint32_t size, uint32_t al, struct iwm_dma_block *b)
{
    struct mock_hw *m = (struct mock_hw *)p;
    void *raw = malloc(size + al);
    if (!raw)
        return IWM_ENOMEM;
    memset(b, 0, sizeof(*b));
    b->alloc_vaddr = raw;
    b->vaddr = (void *)(((size_t)raw + al - 1U) & ~((size_t)al - 1U));
    m->next_phys = IWM_ALIGN_UP(m->next_phys, al);
    b->paddr = m->next_phys;
    m->next_phys += IWM_ALIGN_UP(size, al);
    b->size = size;
    b->alignment = al;
    memset(b->vaddr, 0, size);
    return IWM_OK;
}

static void free_dma(void *p, struct iwm_dma_block *b)
{
    (void)p;
    free(b->alloc_vaddr);
    memset(b, 0, sizeof(*b));
}

static const struct iwm_os_ops ops = {
    mrd, mwr, mdl, malloc_dma, free_dma, NULL, NULL, NULL
};

static void setup(struct mock_hw *hw, struct iwm_adapter *a,
                  struct iwm_rx_queue *q, struct iwm_tx_transport *tx)
{
    memset(hw, 0, sizeof(*hw));
    hw->next_phys = 0x10000000U;
    hw->regs[CSR_HW_REV >> 2] = 0x312U;
    iwm_adapter_init(a, &ops, hw);
    assert(iwm_adapter_probe(a, 0x8086, 0xa370, 0, 54) == IWM_OK);
    a->firmware.catalog.phy_config = 0x00330018UL;
    a->firmware.catalog.calib_flow[IWM_UCODE_TYPE_INIT] = 0x1f8e51dbUL;
    a->firmware.catalog.calib_event[IWM_UCODE_TYPE_INIT] = 0x00000007UL;
    iwm_rx_init(q, a);
    assert(iwm_rx_alloc(q) == IWM_OK);
    assert(iwm_rx_program(q) == IWM_OK);
    q->alive_seen = 1;
    iwm_tx_init(tx, a);
    assert(iwm_tx_alloc(tx) == IWM_OK);
    tx->prepared = 1;
    tx->scheduler_started = 1;
    hw->rxq = q;
    hw->tx = tx;
}

int main(void)
{
    static struct mock_hw hw;
    static struct iwm_init_fw st;
    struct iwm_adapter a;
    struct iwm_rx_queue q;
    struct iwm_tx_transport tx;
    const uint8_t *d;
    uint16_t len = 0;
    unsigned i;

    setup(&hw, &a, &q, &tx);
    iwm_init_fw_init(&st, &a, &q, &tx);
    assert(iwm_init_fw_run(&st) == IWM_OK);
    assert(st.stage == IWM_INIT_DONE && st.init_complete);
    /* Linux order: NVM 0..12 with section 3 read twice, TX ant, PHY cfg, MCC */
    assert(hw.cmds == 17U);
    for (i = 0; i < 14U; ++i)
        assert(hw.order[i] == 0x88);
    assert(hw.order[14] == 0x98 && hw.order[15] == 0x6a && hw.order[16] == 0xc8);
    assert(!st.nvm[0].present && st.nvm[0].status == 0x0100);
    d = iwm_init_fw_nvm(&st, 1, &len);
    assert(d && len == 100 && d[0] == 0x31 && st.nvm[1].chunks == 1);
    d = iwm_init_fw_nvm(&st, 3, &len);
    assert(d && len == 2348 && d[2347] == 0x33 && st.nvm[3].chunks == 2);
    assert(iwm_init_fw_nvm(&st, 10, &len) == NULL);
    assert(st.phydb_count == 3 &&
           st.phydb_used == 4U + 6U + 4U + 10U + 4U + 3080U);
    {
        const uint8_t *e = (const uint8_t *)st.pool.vaddr + IWM_NVM_POOL_SIZE;
        assert(e[0] == 1 && e[2] == 6 && e[4] == 0xab && e[9] == 0xab);
        e += 4U + 6U + 4U + 10U;
        assert(e[0] == 5 && (e[2] | (e[3] << 8)) == 3080 &&
               e[4] == 0xef && e[3083] == 0xef);
    }
    assert(st.notif_other == 2 && st.tx_ant == 3);
    assert(st.other_log[0] == (0x6bUL | (8UL << 16)));
    assert(st.other_log[1] == (0x77UL | (4UL << 16)));
    assert(st.mcc_rc == IWM_OK && st.mcc_len == 20 && st.mcc_resp[4] == 0x53);
    assert(tx.read == tx.write && tx.write == 17U);
    assert(iwm_init_fw_stop(&st) == IWM_OK);
    assert(st.stage == IWM_INIT_STOPPED && a.state == IWM_ADAPTER_PROBED);
    assert(hw.regs[CSR_RESET >> 2] & 0x80U);
    assert(!q.programmed && q.notif_cb == NULL);
    assert(!tx.scheduler_started);
    iwm_init_fw_free(&st);
    iwm_tx_free(&tx);
    iwm_rx_free(&q);

    /* INIT complete never arrives: calibration wait times out. */
    setup(&hw, &a, &q, &tx);
    hw.drop_init_complete = 1;
    iwm_init_fw_init(&st, &a, &q, &tx);
    assert(iwm_init_fw_run(&st) == IWM_ETIMEDOUT);
    assert(st.stage == IWM_INIT_CALIB_WAIT && st.phydb_count == 3);
    iwm_init_fw_free(&st);
    iwm_tx_free(&tx);
    iwm_rx_free(&q);

    /* Not after ALIVE: refused. */
    setup(&hw, &a, &q, &tx);
    q.alive_seen = 0;
    iwm_init_fw_init(&st, &a, &q, &tx);
    assert(iwm_init_fw_run(&st) == IWM_ESTATE);
    iwm_tx_free(&tx);
    iwm_rx_free(&q);

    puts("init firmware tests passed");
    return 0;
}
