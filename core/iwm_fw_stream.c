/* SPDX-License-Identifier: GPL-2.0-only */
/* Ring 0: Dev32lib.h (forced in with -fi) declares memcpy/memset and the
 * miniddk string.h conflicts with it. */
#if !defined(IWM_RING0)
#include <string.h>
#endif
#include "iwm_fw_proto.h"

void iwm_fw_session_init(struct iwm_fw_session *s, struct iwm_blob *store)
{
    memset(s, 0, sizeof(*s));
    s->state = IWM_FW_EMPTY;
    s->store = store;
}

int iwm_fw_begin(struct iwm_fw_session *s, const struct iwm_fw_begin *begin)
{
    if (!s || !begin || begin->magic != IWM_FW_MAGIC ||
        begin->total_length == 0 || begin->total_length > IWM_FW_IMAGE_MAX)
        return IWM_EINVAL;
    /* A new image may only start from an idle or failed session. READY and
     * RUNNING images must not be overwritten; stop the device (or ABORT a
     * READY image) first. */
    if (s->state != IWM_FW_EMPTY && s->state != IWM_FW_FAILED)
        return IWM_EBUSY;
    memset(&s->catalog, 0, sizeof(s->catalog));
    if (s->store) {
        int rc = iwm_blob_resize(s->store, begin->total_length);
        if (rc != IWM_OK) {
            s->last_error = rc;
            s->state = IWM_FW_FAILED;
            return rc;
        }
    }
    s->total_length = begin->total_length;
    s->expected_crc32 = begin->expected_crc32;
    s->running_crc32 = 0xFFFFFFFFUL;
    s->accepted = 0;
    s->next_sequence = 0;
    s->last_error = IWM_OK;
    s->state = IWM_FW_RECEIVING;
    return IWM_OK;
}

int iwm_fw_push(struct iwm_fw_session *s, const struct iwm_fw_chunk *chunk)
{
    int rc;
    if (!s || !chunk || s->state != IWM_FW_RECEIVING)
        return IWM_ESTATE;
    if (chunk->length == 0 || chunk->length > IWM_FW_CHUNK_MAX ||
        chunk->sequence != s->next_sequence || chunk->offset != s->accepted ||
        s->accepted > s->total_length ||
        (uint32_t)chunk->length > s->total_length - s->accepted) {
        s->last_error = IWM_EINVAL;
        return IWM_EINVAL;
    }
    if (s->store) {
        rc = iwm_blob_write(s->store, chunk->offset,
                            chunk->data, chunk->length);
        if (rc != IWM_OK) {
            s->last_error = rc;
            s->state = IWM_FW_FAILED;
            return rc;
        }
    }
    s->running_crc32 = iwm_crc32_update(s->running_crc32,
                                        chunk->data, chunk->length);
    s->accepted += chunk->length;
    ++s->next_sequence;
    return IWM_OK;
}

int iwm_fw_commit(struct iwm_fw_session *s)
{
    uint32_t actual;
    if (!s || s->state != IWM_FW_RECEIVING)
        return IWM_ESTATE;
    if (s->accepted != s->total_length) {
        s->last_error = IWM_EOVERFLOW;
        return IWM_EOVERFLOW;
    }
    actual = s->running_crc32 ^ 0xFFFFFFFFUL;
    /* Always verify; an expected CRC of zero is not a bypass. */
    if (actual != s->expected_crc32) {
        s->last_error = IWM_ECRC;
        s->state = IWM_FW_FAILED;
        return IWM_ECRC;
    }
    if (s->store) {
        int rc = iwm_fw_image_parse(s->store, &s->catalog);
        if (rc == IWM_OK && s->catalog.runtime_count == 0) {
            s->catalog.diag.reason = IWM_FWP_NO_RUNTIME;
            rc = IWM_EFORMAT;
        }
        if (rc != IWM_OK) {
            s->last_error = rc;
            s->state = IWM_FW_FAILED;
            return rc;
        }
    }
    s->state = IWM_FW_READY;
    s->last_error = IWM_OK;
    return IWM_OK;
}

int iwm_fw_abort(struct iwm_fw_session *s, int error)
{
    if (!s)
        return IWM_EINVAL;
    if (error == IWM_OK &&
        (s->state == IWM_FW_LOADING || s->state == IWM_FW_RUNNING))
        return IWM_EBUSY;
    s->accepted = 0;
    s->next_sequence = 0;
    s->last_error = error;
    s->state = error == IWM_OK ? IWM_FW_EMPTY : IWM_FW_FAILED;
    memset(&s->catalog, 0, sizeof(s->catalog));
    iwm_blob_reset(s->store);
    return IWM_OK;
}

int iwm_fw_start_load(struct iwm_fw_session *s)
{
    if (!s || s->state != IWM_FW_READY)
        return IWM_ESTATE;
    /* Nothing can be loaded without a retained image and a runtime section. */
    if (!s->store || s->catalog.runtime_count == 0)
        return IWM_ENOTSUP;
    s->state = IWM_FW_LOADING;
    s->last_error = IWM_OK;
    return IWM_OK;
}

int iwm_fw_mark_running(struct iwm_fw_session *s)
{
    if (!s || s->state != IWM_FW_LOADING)
        return IWM_ESTATE;
    s->state = IWM_FW_RUNNING;
    s->last_error = IWM_OK;
    return IWM_OK;
}

void iwm_fw_get_status(const struct iwm_fw_session *s,
                       struct iwm_fw_status *status)
{
    if (!s || !status)
        return;
    status->accepted = s->accepted;
    status->running_crc32 = s->running_crc32 ^ 0xFFFFFFFFUL;
    status->last_error = s->last_error;
    status->next_sequence = s->next_sequence;
    status->state = s->state;
}
