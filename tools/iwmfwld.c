/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "iwm_fw_proto.h"

#if defined(__OS2__)
#define INCL_DOSFILEMGR
#define INCL_DOSDEVICES
#include <os2.h>

static APIRET ioctl_data(HFILE h, ULONG fn, void *data, ULONG *length)
{
    ULONG parm_len = 0;
    return DosDevIOCtl(h, IWM_IOCTL_CATEGORY, fn, NULL, 0, &parm_len,
                       data, *length, length);
}

static int load_firmware(const char *device, const char *path)
{
    FILE *f;
    HFILE h;
    ULONG action, open_mode, data_len;
    APIRET rc;
    long file_length;
    uint32_t crc = 0xFFFFFFFFUL;
    uint8_t scan[16384];
    size_t n;
    struct iwm_fw_query query;
    struct iwm_fw_begin begin;
    struct iwm_fw_chunk *chunk;
    uint32_t offset = 0;
    uint16_t sequence = 0;

    f = fopen(path, "rb");
    if (!f) { perror(path); return 2; }
    if (fseek(f, 0, SEEK_END) || (file_length = ftell(f)) <= 0 ||
        fseek(f, 0, SEEK_SET)) { fclose(f); return 2; }
    while ((n = fread(scan, 1, sizeof(scan), f)) != 0)
        crc = iwm_crc32_update(crc, scan, n);
    crc ^= 0xFFFFFFFFUL;
    rewind(f);

    open_mode = OPEN_ACCESS_READWRITE | OPEN_SHARE_DENYNONE;
    rc = DosOpen((PSZ)device, &h, &action, 0, FILE_NORMAL,
                 OPEN_ACTION_OPEN_IF_EXISTS, open_mode, NULL);
    if (rc) { fprintf(stderr, "DosOpen(%s) failed: %lu\n", device, rc); fclose(f); return 3; }

    memset(&query, 0, sizeof(query));
    data_len = sizeof(query);
    rc = ioctl_data(h, IWM_IOCTL_FW_QUERY, &query, &data_len);
    if (rc || query.protocol_version != IWM_FW_PROTOCOL_VERSION) {
        fprintf(stderr, "firmware protocol query failed: %lu\n", rc);
        DosClose(h); fclose(f); return 4;
    }
    if ((uint32_t)file_length > query.max_image_size) {
        fprintf(stderr, "firmware image is too large\n");
        DosClose(h); fclose(f); return 4;
    }

    memset(&begin, 0, sizeof(begin));
    begin.magic = IWM_FW_MAGIC;
    begin.total_length = (uint32_t)file_length;
    begin.expected_crc32 = crc;
    data_len = sizeof(begin);
    rc = ioctl_data(h, IWM_IOCTL_FW_BEGIN, &begin, &data_len);
    if (rc) { fprintf(stderr, "FW_BEGIN failed: %lu\n", rc); DosClose(h); fclose(f); return 5; }

    chunk = (struct iwm_fw_chunk *)malloc(sizeof(*chunk));
    if (!chunk) { DosClose(h); fclose(f); return 6; }
    while ((n = fread(chunk->data, 1, query.max_chunk_size, f)) != 0) {
        chunk->offset = offset;
        chunk->length = (uint16_t)n;
        chunk->sequence = sequence;
        data_len = (ULONG)(8 + n);
        rc = ioctl_data(h, IWM_IOCTL_FW_DATA, chunk, &data_len);
        if (rc) {
            fprintf(stderr, "FW_DATA failed at %lu: %lu\n", (ULONG)offset, rc);
            data_len = 0;
            ioctl_data(h, IWM_IOCTL_FW_ABORT, NULL, &data_len);
            free(chunk); DosClose(h); fclose(f); return 7;
        }
        offset += (uint32_t)n;
        ++sequence;
    }
    free(chunk);
    fclose(f);
    data_len = 0;
    rc = ioctl_data(h, IWM_IOCTL_FW_COMMIT, NULL, &data_len);
    DosClose(h);
    if (rc) { fprintf(stderr, "FW_COMMIT failed: %lu\n", rc); return 8; }
    printf("Accepted %lu bytes for Intel device %04x, CRC32 %08lx\n",
           (ULONG)offset, query.device_id, (ULONG)crc);
    return 0;
}

int main(int argc, char **argv)
{
    const char *device = "\\\\DEV\\WRND32$";
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: iwmfwld firmware.ucode [device]\n");
        return 1;
    }
    if (argc == 3) device = argv[2];
    return load_firmware(device, argv[1]);
}
#else
int main(void)
{
    fputs("iwmfwld is an OS/2 ring-3 program; build it with Open Watcom on OS/2.\n", stderr);
    return 1;
}
#endif
