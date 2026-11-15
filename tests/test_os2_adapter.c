/* SPDX-License-Identifier: GPL-2.0-only */
/* Attach ordering, rollback and idempotent detach of the OS/2 bridge. */
#include "assert.h"
#include "stdio.h"
#include "string.h"
#include "iwm_os2_adapter.h"
#include "iwm_hw.h"

struct trace {
    int fail_at;      /* 1..4: which attach step fails, 0: none */
    int live[5];      /* resources currently held, by step */
    int releases;
};

static int step(struct trace *t, int n)
{
    if (t->fail_at == n)
        return IWM_EIO;
    assert(!t->live[n]);
    t->live[n] = 1;
    return IWM_OK;
}
static void undo(struct trace *t, int n)
{
    assert(t->live[n]);
    t->live[n] = 0;
    ++t->releases;
}
static int pa(void *c, struct iwm_adapter *a) { (void)a; return step(c, 1); }
static void pd(void *c, struct iwm_adapter *a) { (void)a; undo(c, 1); }
static int mb(void *c, struct iwm_adapter *a) { (void)a; return step(c, 2); }
static void ub(void *c, struct iwm_adapter *a) { (void)a; undo(c, 2); }
static int bs(void *c, struct iwm_adapter *a) { (void)a; return step(c, 3); }
static void rs(void *c, struct iwm_adapter *a) { (void)a; undo(c, 3); }
static int ii(void *c, struct iwm_adapter *a) { (void)a; return step(c, 4); }
static void ri(void *c, struct iwm_adapter *a) { (void)a; undo(c, 4); }

static const struct iwm_os2_target_api full = { pa, pd, mb, ub, bs, rs, ii, ri };
static const struct iwm_os2_target_api partial = { pa, pd, 0, 0, 0, 0, 0, 0 };

int main(void)
{
    struct iwm_adapter a;
    struct iwm_os2_target t;
    struct trace tr;
    int f, i;

    memset(&a, 0, sizeof(a));

    /* Unverified interfaces stay disabled rather than half-working. */
    memset(&t, 0, sizeof(t));
    t.api = &partial;
    assert(iwm_os2_target_attach(&t, &a) == IWM_ENOTSUP && t.stage == 0);

    /* Failure at each step releases exactly what was acquired. */
    for (f = 1; f <= 4; ++f) {
        memset(&tr, 0, sizeof(tr));
        tr.fail_at = f;
        memset(&t, 0, sizeof(t));
        t.api = &full;
        t.context = &tr;
        assert(iwm_os2_target_attach(&t, &a) == IWM_EIO);
        assert(t.stage == 0 && tr.releases == f - 1);
        for (i = 1; i <= 4; ++i)
            assert(!tr.live[i]);
    }

    /* Full attach, double attach refused, detach twice is harmless. */
    memset(&tr, 0, sizeof(tr));
    memset(&t, 0, sizeof(t));
    t.api = &full;
    t.context = &tr;
    assert(iwm_os2_target_attach(&t, &a) == IWM_OK && t.stage == 4);
    assert(iwm_os2_target_attach(&t, &a) == IWM_EBUSY);
    iwm_os2_target_detach(&t, &a);
    iwm_os2_target_detach(&t, &a);
    assert(t.stage == 0 && tr.releases == 4);
    assert(a.firmware.store == NULL);

    puts("os2 adapter tests passed");
    return 0;
}


