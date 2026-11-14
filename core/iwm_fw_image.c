/* SPDX-License-Identifier: GPL-2.0-only */
#include "string.h"
#include "iwm_fw_image.h"

static int span_ok(uint32_t offset, uint32_t length, uint32_t total)
{
    return offset <= total && length <= total - offset;
}

void iwm_blob_bind(struct iwm_blob *blob,
                   const struct iwm_blob_ops *ops, void *context)
{
    if (!blob)
        return;
    blob->ops = ops;
    blob->context = context;
    blob->length = 0;
}

int iwm_blob_resize(struct iwm_blob *blob, uint32_t length)
{
    int rc;
    if (!blob || !blob->ops || !blob->ops->resize)
        return IWM_EINVAL;
    rc = blob->ops->resize(blob->context, length);
    blob->length = rc == IWM_OK ? length : 0;
    return rc;
}

int iwm_blob_write(struct iwm_blob *blob, uint32_t offset,
                   const uint8_t *data, uint16_t length)
{
    if (!blob || !data || length == 0 || !blob->ops || !blob->ops->write ||
        !span_ok(offset, length, blob->length))
        return IWM_EINVAL;
    return blob->ops->write(blob->context, offset, data, length);
}

int iwm_blob_read(const struct iwm_blob *blob, uint32_t offset,
                  uint8_t *data, uint16_t length)
{
    if (!blob || !data || length == 0 || !blob->ops || !blob->ops->read ||
        !span_ok(offset, length, blob->length))
        return IWM_EINVAL;
    return blob->ops->read(blob->context, offset, data, length);
}

void iwm_blob_reset(struct iwm_blob *blob)
{
    if (!blob)
        return;
    if (blob->ops && blob->ops->reset)
        blob->ops->reset(blob->context);
    blob->length = 0;
}

static int classify(uint32_t type, uint8_t *flags)
{
    switch (type) {
    case IWM_UCODE_TLV_SEC_RT:
        *flags = IWM_FW_SEC_RUNTIME;
        return 1;
    case IWM_UCODE_TLV_SEC_INIT:
        *flags = IWM_FW_SEC_INIT;
        return 1;
    case IWM_UCODE_TLV_SEC_WOWLAN:
        *flags = IWM_FW_SEC_WOWLAN;
        return 1;
    case IWM_UCODE_TLV_SECURE_SEC_RT:
        *flags = IWM_FW_SEC_RUNTIME | IWM_FW_SEC_SECURE;
        return 1;
    case IWM_UCODE_TLV_SECURE_SEC_INIT:
        *flags = IWM_FW_SEC_INIT | IWM_FW_SEC_SECURE;
        return 1;
    default:
        return 0;
    }
}

int iwm_fw_image_parse(const struct iwm_blob *blob,
                       struct iwm_fw_image *image)
{
    uint8_t hdr[8];
    uint8_t word[4];
    uint32_t total, pos, type, length, padded, payload, dst;
    uint8_t flags;
    struct iwm_fw_section *sec;

    if (!blob || !image)
        return IWM_EINVAL;
    memset(image, 0, sizeof(*image));
    total = blob->length;
    if (total < IWM_UCODE_HEADER_SIZE)
        return IWM_EFORMAT;
    if (iwm_blob_read(blob, 0, hdr, 8) != IWM_OK)
        return IWM_EIO;
    if (iwm_get_le32(hdr) != 0 ||
        iwm_get_le32(hdr + 4) != IWM_UCODE_TLV_MAGIC)
        return IWM_EFORMAT;
    if (iwm_blob_read(blob, IWM_UCODE_VER_OFFSET, word, 4) != IWM_OK)
        return IWM_EIO;
    image->version = iwm_get_le32(word);
    if (iwm_blob_read(blob, IWM_UCODE_BUILD_OFFSET, word, 4) != IWM_OK)
        return IWM_EIO;
    image->build = iwm_get_le32(word);

    pos = IWM_UCODE_HEADER_SIZE;
    while (pos != total) {
        if (total - pos < 8U)
            return IWM_EFORMAT;
        if (iwm_blob_read(blob, pos, hdr, 8) != IWM_OK)
            return IWM_EIO;
        type = iwm_get_le32(hdr);
        length = iwm_get_le32(hdr + 4);
        pos += 8U;
        if (length > total - pos)
            return IWM_EFORMAT;
        padded = (length + 3U) & ~3UL;
        /* The last record may omit its padding; others may not. */
        if (padded > total - pos)
            padded = length;
        if (type == IWM_UCODE_TLV_NUM_OF_CPU) {
            if (length != 4U)
                return IWM_EFORMAT;
            if (iwm_blob_read(blob, pos, word, 4) != IWM_OK)
                return IWM_EIO;
            image->num_cpus = iwm_get_le32(word);
            if (image->num_cpus == 0U || image->num_cpus > 2U)
                return IWM_EFORMAT;
        } else if (classify(type, &flags)) {
            if (length < 4U)
                return IWM_EFORMAT;
            if (image->section_count >= IWM_FW_MAX_SECTIONS)
                return IWM_EOVERFLOW;
            if (iwm_blob_read(blob, pos, word, 4) != IWM_OK)
                return IWM_EIO;
            dst = iwm_get_le32(word);
            payload = length - 4U;
            if (dst == IWM_UCODE_SECTION_CPU1_CPU2 ||
                dst == IWM_UCODE_SECTION_PAGING) {
                if (payload != 0U)
                    return IWM_EFORMAT;
                flags |= IWM_FW_SEC_SEPARATOR;
            } else {
                if (payload == 0U)
                    return IWM_EFORMAT;
                if (dst > 0xFFFFFFFFUL - (payload - 1U))
                    return IWM_EOVERFLOW;
                if (flags & IWM_FW_SEC_INIT)
                    ++image->init_count;
                else if (flags & IWM_FW_SEC_RUNTIME)
                    ++image->runtime_count;
            }
            sec = &image->section[image->section_count++];
            sec->tlv_type = type;
            sec->dst_addr = dst;
            sec->data_offset = pos + 4U;
            sec->data_length = payload;
            sec->flags = flags;
        }
        pos += padded;
    }
    return IWM_OK;
}
