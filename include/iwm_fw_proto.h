/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef IWM_FW_PROTO_H
#define IWM_FW_PROTO_H

#include "iwm_types.h"
#include "iwm_fw_image.h"

/* Private ring-3 firmware-loader category. GenMAC/XWLAN keeps category 0x99. */
#define IWM_IOCTL_CATEGORY       0x91U
#define IWM_IOCTL_FW_QUERY       0x00U
#define IWM_IOCTL_FW_BEGIN       0x01U
#define IWM_IOCTL_FW_DATA        0x02U
#define IWM_IOCTL_FW_COMMIT      0x03U
#define IWM_IOCTL_FW_ABORT       0x04U
#define IWM_IOCTL_FW_STATUS      0x05U

#define IWM_FW_PROTOCOL_VERSION  0x00010000UL
#define IWM_FW_CHUNK_MAX         16384U
#define IWM_FW_IMAGE_MAX         (4UL * 1024UL * 1024UL)
#define IWM_FW_MAGIC             0x314D5749UL /* "IWM1" in little endian */

enum iwm_fw_state {
    IWM_FW_EMPTY = 0,
    IWM_FW_RECEIVING,
    IWM_FW_READY,
    IWM_FW_LOADING,
    IWM_FW_RUNNING,
    IWM_FW_FAILED
};

struct IWM_PACKED iwm_fw_query {
    uint32_t protocol_version;
    uint32_t max_image_size;
    uint16_t max_chunk_size;
    uint16_t device_id;
    uint16_t family;
    uint16_t state;
};

struct IWM_PACKED iwm_fw_begin {
    uint32_t magic;
    uint32_t total_length;
    uint32_t expected_crc32;
    uint32_t flags;
};

struct IWM_PACKED iwm_fw_chunk {
    uint32_t offset;
    uint16_t length;
    uint16_t sequence;
    uint8_t data[IWM_FW_CHUNK_MAX];
};

/* Bytes of struct iwm_fw_chunk that precede data[]; a DATA IOCTL must supply
 * at least this header plus chunk->length payload bytes. */
#define IWM_FW_CHUNK_HEADER      offsetof(struct iwm_fw_chunk, data)

struct IWM_PACKED iwm_fw_status {
    uint32_t accepted;
    uint32_t running_crc32;
    int32_t last_error;
    uint16_t next_sequence;
    uint16_t state;
};

/*
 * Every accepted DATA record is copied into the bound firmware store before
 * the CRC advances; COMMIT verifies the CRC and catalogs the image. A session
 * without a store validates the stream only and refuses to start loading.
 */
struct iwm_fw_session {
    uint32_t total_length;
    uint32_t expected_crc32;
    uint32_t running_crc32;
    uint32_t accepted;
    uint16_t next_sequence;
    uint16_t state;
    int32_t last_error;
    struct iwm_blob *store;
    struct iwm_fw_image catalog;
};

uint32_t iwm_crc32_update(uint32_t crc, const void *data, size_t length);
void iwm_fw_session_init(struct iwm_fw_session *s, struct iwm_blob *store);
int iwm_fw_begin(struct iwm_fw_session *s, const struct iwm_fw_begin *begin);
int iwm_fw_push(struct iwm_fw_session *s, const struct iwm_fw_chunk *chunk);
int iwm_fw_commit(struct iwm_fw_session *s);
/* error == IWM_OK: caller-requested abort, refused while LOADING/RUNNING.
 * error != IWM_OK: driver-internal failure, always forces IWM_FW_FAILED. */
int iwm_fw_abort(struct iwm_fw_session *s, int error);
/* READY -> LOADING once the staged image is handed to the hardware loader.
 * Returns IWM_ENOTSUP when no firmware store is bound. */
int iwm_fw_start_load(struct iwm_fw_session *s);
/* LOADING -> RUNNING when the runtime image reports ALIVE. */
int iwm_fw_mark_running(struct iwm_fw_session *s);
void iwm_fw_get_status(const struct iwm_fw_session *s,
                       struct iwm_fw_status *status);

#endif

