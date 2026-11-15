/* SPDX-License-Identifier: GPL-2.0-only */
#include "iwm_os2_adapter.h"
#include "iwm_hw.h"

int iwm_os2_target_attach(struct iwm_os2_target *t,
                          struct iwm_adapter *a)
{
    const struct iwm_os2_target_api *o;
    int rc;

    if (!t || !a)
        return IWM_EINVAL;
    o = t->api;
    if (!o || !o->pci_attach || !o->pci_detach || !o->map_bar ||
        !o->unmap_bar || !o->bind_fw_store || !o->release_fw_store ||
        !o->install_irq || !o->remove_irq)
        return IWM_ENOTSUP; /* unverified target interfaces stay disabled */
    if (t->stage != 0)
        return IWM_EBUSY;

    rc = o->pci_attach(t->context, a);
    if (rc != IWM_OK)
        goto fail;
    t->stage = 1;
    rc = o->map_bar(t->context, a);
    if (rc != IWM_OK)
        goto fail;
    t->stage = 2;
    rc = o->bind_fw_store(t->context, a);
    if (rc != IWM_OK)
        goto fail;
    t->stage = 3;
    rc = o->install_irq(t->context, a);
    if (rc != IWM_OK)
        goto fail;
    t->stage = 4;
    return IWM_OK;
fail:
    iwm_os2_target_detach(t, a);
    return rc;
}

void iwm_os2_target_detach(struct iwm_os2_target *t, struct iwm_adapter *a)
{
    const struct iwm_os2_target_api *o;

    if (!t || !t->api || !a)
        return;
    o = t->api;
    /* Reverse order: no interrupt may run while DMA memory is released. */
    if (t->stage >= 4) {
        o->remove_irq(t->context, a);
        t->stage = 3;
    }
    if (t->stage >= 3) {
        iwm_adapter_set_fw_store(a, NULL);
        o->release_fw_store(t->context, a);
        t->stage = 2;
    }
    if (t->stage >= 2) {
        o->unmap_bar(t->context, a);
        t->stage = 1;
    }
    if (t->stage >= 1) {
        o->pci_detach(t->context, a);
        t->stage = 0;
    }
}

