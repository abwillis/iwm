/* SPDX-License-Identifier: GPL-2.0-only */
/* Firmware store and TLV parser tests, including hostile input. */
#include "assert.h"
#include "stdio.h"
#include "iwm_test_blob.h"

static int parse_bytes(struct iwm_blob *b, const uint8_t *img, uint32_t len,
                       struct iwm_fw_image *out)
{
    assert(iwm_blob_resize(b, len) == IWM_OK);
    if (len)
        assert(iwm_blob_write(b, 0, img, (uint16_t)len) == IWM_OK);
    return iwm_fw_image_parse(b, out);
}

int main(void)
{
    uint8_t img[160];
    uint8_t tmp[160];
    struct mem_store ms;
    struct iwm_blob b;
    struct iwm_fw_image fw;
    uint32_t len, i;

    memset(&ms, 0, sizeof(ms));
    iwm_blob_bind(&b, &mem_store_ops, &ms);

    /* Well-formed image: header, CPU count, INIT, separator, RT. */
    len = build_image(img, 1);
    assert(parse_bytes(&b, img, len, &fw) == IWM_OK);
    assert(fw.version == 46 && fw.build == 0x11223344UL && fw.num_cpus == 2);
    assert(fw.section_count == 3 && fw.init_count == 1 && fw.runtime_count == 1);
    assert(fw.section[0].flags == (IWM_FW_SEC_INIT | IWM_FW_SEC_SECURE));
    assert(fw.section[1].flags & IWM_FW_SEC_SEPARATOR);
    assert(fw.section[2].dst_addr == 0x00800000UL);
    assert(fw.section[2].data_length == 4);
    for (i = 0; i < 4; ++i)
        assert(ms.buf[fw.section[2].data_offset + i] == img[len - 4 + i]);

    /* Every truncation is either rejected or, when it ends on a record
     * boundary, yields no runtime section (which COMMIT then rejects). */
    for (i = 0; i < len; ++i) {
        int rc = parse_bytes(&b, img, i, &fw);
        assert(rc == IWM_EFORMAT || (rc == IWM_OK && fw.runtime_count == 0));
    }

    /* Bad zero word and bad magic. */
    memcpy(tmp, img, len);
    put_le32(tmp, 1);
    assert(parse_bytes(&b, tmp, len, &fw) == IWM_EFORMAT);
    memcpy(tmp, img, len);
    put_le32(tmp + 4, 0);
    assert(parse_bytes(&b, tmp, len, &fw) == IWM_EFORMAT);

    /* TLV length beyond the image and near 4 GiB. */
    memcpy(tmp, img, len);
    put_le32(tmp + IWM_UCODE_HEADER_SIZE + 4, 0xFFFFFFFFUL);
    assert(parse_bytes(&b, tmp, len, &fw) == IWM_EFORMAT);
    put_le32(tmp + IWM_UCODE_HEADER_SIZE + 4, 0xFFFFFFFDUL);
    assert(parse_bytes(&b, tmp, len, &fw) == IWM_EFORMAT);

    /* NUM_OF_CPU out of range or wrong size. */
    memcpy(tmp, img, len);
    put_le32(tmp + IWM_UCODE_HEADER_SIZE + 8, 3);
    assert(parse_bytes(&b, tmp, len, &fw) == IWM_EFORMAT);

    /* Destination wrap past 4 GiB. */
    memcpy(tmp, img, len);
    put_le32(tmp + len - 8, 0xFFFFFFFEUL);
    assert(parse_bytes(&b, tmp, len, &fw) == IWM_EOVERFLOW);

    /* Separator carrying payload is malformed. */
    memcpy(tmp, img, len);
    put_le32(tmp + 116 + 8, IWM_UCODE_SECTION_PAGING);
    put_le32(tmp + 116 + 4, 4);
    assert(parse_bytes(&b, tmp, len, &fw) == IWM_OK);
    memcpy(tmp, img, len);
    put_le32(tmp + IWM_UCODE_HEADER_SIZE + 12 + 8, IWM_UCODE_SECTION_PAGING);
    assert(parse_bytes(&b, tmp, len, &fw) == IWM_EFORMAT);

    /* Trailing bytes too short for a TLV header. */
    memcpy(tmp, img, len);
    assert(parse_bytes(&b, tmp, len + 4, &fw) == IWM_EFORMAT);

    /* Store bounds: writes and reads beyond the sized image fail. */
    assert(iwm_blob_resize(&b, 16) == IWM_OK);
    assert(iwm_blob_write(&b, 12, img, 8) == IWM_EINVAL);
    assert(iwm_blob_read(&b, 0xFFFFFFF0UL, tmp, 32) == IWM_EINVAL);
    assert(iwm_blob_write(&b, 0, img, 0) == IWM_EINVAL);

    /* Reset is idempotent. */
    iwm_blob_reset(&b);
    iwm_blob_reset(&b);
    assert(b.length == 0 && ms.buf == NULL && ms.resets == 2);

    puts("fw image tests passed");
    return 0;
}
