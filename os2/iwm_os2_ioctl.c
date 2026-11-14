/* SPDX-License-Identifier: GPL-2.0-only */
#include <string.h>
#include "iwm_os2.h"

static int copy_out(struct iwm_os2_ioctl *r, const void *p, uint32_t n)
{
    if (!r->data || r->data_length < n)
        return IWM_EOVERFLOW;
    memcpy(r->data, p, n);
    r->data_actual = n;
    return IWM_OK;
}

int iwm_os2_ioctl_dispatch(struct iwm_adapter *a,
                           struct iwm_os2_ioctl *r)
{
    struct iwm_fw_query q;
    struct iwm_fw_status status;
    struct iwm_oid_request oid;
    const struct iwm_fw_chunk *chunk;
    int rc;
    if (!a || !r)
        return IWM_EINVAL;
    if (r->category == IWM_IOCTL_CATEGORY) {
        switch (r->function) {
        case IWM_IOCTL_FW_QUERY:
            memset(&q, 0, sizeof(q));
            q.protocol_version = IWM_FW_PROTOCOL_VERSION;
            q.max_image_size = IWM_FW_IMAGE_MAX;
            q.max_chunk_size = IWM_FW_CHUNK_MAX;
            q.device_id = a->cfg ? a->cfg->device_id : 0;
            q.family = a->cfg ? a->cfg->family : 0;
            q.state = a->firmware.state;
            return copy_out(r, &q, sizeof(q));
        case IWM_IOCTL_FW_BEGIN:
            if (!r->data || r->data_length < sizeof(struct iwm_fw_begin))
                return IWM_EINVAL;
            return iwm_fw_begin(&a->firmware,
                                (const struct iwm_fw_begin *)r->data);
        case IWM_IOCTL_FW_DATA:
            if (!r->data || r->data_length < IWM_FW_CHUNK_HEADER)
                return IWM_EINVAL;
            chunk = (const struct iwm_fw_chunk *)r->data;
            /* Never read past the bytes the caller actually supplied. */
            if (chunk->length > IWM_FW_CHUNK_MAX ||
                r->data_length < IWM_FW_CHUNK_HEADER + (uint32_t)chunk->length)
                return IWM_EINVAL;
            return iwm_fw_push(&a->firmware, chunk);
        case IWM_IOCTL_FW_COMMIT:
            rc = iwm_fw_commit(&a->firmware);
            if (rc == IWM_OK)
                rc = iwm_fw_start_load(&a->firmware);
            if (rc == IWM_OK) {
                /* Milestone hook: parse TLVs, load init/runtime images via FH
                 * DMA. The ALIVE handler calls iwm_adapter_fw_alive(); load
                 * errors or ALIVE timeout call iwm_fw_abort(fw, error). */
                a->state = IWM_ADAPTER_FW_WAIT;
            }
            return rc;
        case IWM_IOCTL_FW_ABORT:
            return iwm_fw_abort(&a->firmware, IWM_OK);
        case IWM_IOCTL_FW_STATUS:
            iwm_fw_get_status(&a->firmware, &status);
            return copy_out(r, &status, sizeof(status));
        default:
            return IWM_ENOTSUP;
        }
    }
    if (r->category != GENMAC_CATEGORY ||
        (r->function != GENMAC_WRAPPER_OID_GET &&
         r->function != GENMAC_WRAPPER_OID_SET))
        return IWM_ENOTSUP;
    oid.oid = r->parameters[0];
    oid.data = r->data;
    oid.data_length = r->data_length;
    oid.bytes_done = 0;
    oid.ndis_status = 0;
    rc = r->function == GENMAC_WRAPPER_OID_GET ?
         iwm_genmac_oid_get(a, &oid) : iwm_genmac_oid_set(a, &oid);
    r->parameters[2] = oid.bytes_done;
    r->parameters[3] = oid.ndis_status;
    r->data_actual = oid.bytes_done;
    return rc;
}
