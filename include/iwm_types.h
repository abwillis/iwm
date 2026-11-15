/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef IWM_TYPES_H
#define IWM_TYPES_H

/* Ring 0: Dev32lib.h already provides size_t and offsetof, and the miniddk
 * stddef.h redefines them. Everything else keeps the standard header. */
#if !defined(IWM_RING0)
#include <stddef.h>
#endif
#if defined(__WATCOMC__) && defined(__OS2__)
typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned long  uint32_t;
typedef signed long    int32_t;
#else
#include <stdint.h>
#endif

#if defined(__WATCOMC__)
#define IWM_PACKED
#define IWM_INLINE __inline
#else
#define IWM_PACKED __attribute__((packed))
#define IWM_INLINE inline
#endif

#define IWM_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define IWM_MIN(a,b) ((a) < (b) ? (a) : (b))
#define IWM_ALIGN_UP(v,a) (((v) + ((a) - 1U)) & ~((a) - 1U))

#ifndef NULL
#define NULL ((void *)0)
#endif

enum iwm_status {
    IWM_OK = 0,
    IWM_EINVAL = -1,
    IWM_ENODEV = -2,
    IWM_ENOMEM = -3,
    IWM_EBUSY = -4,
    IWM_EIO = -5,
    IWM_ETIMEDOUT = -6,
    IWM_ESTATE = -7,
    IWM_ECRC = -8,
    IWM_ENOTSUP = -9,
    IWM_EOVERFLOW = -10,
    IWM_EFORMAT = -11,
    IWM_ERANGE = -12
};

static IWM_INLINE uint16_t iwm_get_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static IWM_INLINE uint32_t iwm_get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#endif


