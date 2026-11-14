/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef IWM_FW_IMAGE_H
#define IWM_FW_IMAGE_H

#include "iwm_types.h"

/*
 * Portable Intel firmware-image model.
 *
 * The complete .ucode image is never placed in a 16-bit data segment and no
 * ring-3 pointer is ever retained. The OS/2 layer backs a "blob" with fixed
 * physical memory reached through bounded GDT selector windows; host tests
 * back it with ordinary memory. Section records hold byte offsets into the
 * staged image, never pointers.
 *
 * TLV layout follows current Linux iwlwifi fw/file.h: a zero word, the
 * magic 0x0a4c5749, a 64-byte human-readable string, version and build
 * words, then four-byte-padded type/length records.
 */
#define IWM_UCODE_TLV_MAGIC          0x0A4C5749UL
#define IWM_UCODE_HEADER_SIZE        88U
#define IWM_UCODE_VER_OFFSET         72U
#define IWM_UCODE_BUILD_OFFSET       76U
#define IWM_FW_MAX_SECTIONS          64U

#define IWM_UCODE_TLV_SEC_RT         19UL
#define IWM_UCODE_TLV_SEC_INIT       20UL
#define IWM_UCODE_TLV_SEC_WOWLAN     21UL
#define IWM_UCODE_TLV_SECURE_SEC_RT  24UL
#define IWM_UCODE_TLV_SECURE_SEC_INIT 25UL
#define IWM_UCODE_TLV_NUM_OF_CPU     27UL

/* Zero-length separator sections used by 8000/9000-family images. */
#define IWM_UCODE_SECTION_CPU1_CPU2  0xFFFFCCCCUL
#define IWM_UCODE_SECTION_PAGING     0xAAAABBBBUL

#define IWM_FW_SEC_RUNTIME           0x01U
#define IWM_FW_SEC_INIT              0x02U
#define IWM_FW_SEC_WOWLAN            0x04U
#define IWM_FW_SEC_SECURE            0x08U
#define IWM_FW_SEC_SEPARATOR         0x10U

struct iwm_blob_ops {
    int (*resize)(void *context, uint32_t length);
    int (*write)(void *context, uint32_t offset,
                 const uint8_t *data, uint16_t length);
    int (*read)(void *context, uint32_t offset,
                uint8_t *data, uint16_t length);
    void (*reset)(void *context);
};

struct iwm_blob {
    const struct iwm_blob_ops *ops;
    void *context;
    uint32_t length;
};

struct iwm_fw_section {
    uint32_t tlv_type;
    uint32_t dst_addr;
    uint32_t data_offset;
    uint32_t data_length;
    uint8_t flags;
};

struct iwm_fw_image {
    uint32_t version;
    uint32_t build;
    uint32_t num_cpus;
    uint16_t section_count;
    uint16_t runtime_count;
    uint16_t init_count;
    struct iwm_fw_section section[IWM_FW_MAX_SECTIONS];
};

void iwm_blob_bind(struct iwm_blob *blob,
                   const struct iwm_blob_ops *ops, void *context);
int iwm_blob_resize(struct iwm_blob *blob, uint32_t length);
int iwm_blob_write(struct iwm_blob *blob, uint32_t offset,
                   const uint8_t *data, uint16_t length);
int iwm_blob_read(const struct iwm_blob *blob, uint32_t offset,
                  uint8_t *data, uint16_t length);
void iwm_blob_reset(struct iwm_blob *blob);

/* Validate the TLV container and catalog every firmware section. */
int iwm_fw_image_parse(const struct iwm_blob *blob,
                       struct iwm_fw_image *image);

#endif
