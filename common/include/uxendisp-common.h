/*
 * Copyright 2015-2017, Bromium, Inc.
 * Author: Piotr Foltyn <piotr.foltyn@gmail.com>
 * SPDX-License-Identifier: ISC
 */

#ifndef _DISP_H_
#define _DISP_H_

#if !defined(_MSC_VER)
#include <stdint.h>
#else
typedef __int32 int32_t;
typedef unsigned __int32 uint32_t;
typedef unsigned __int64 uint64_t;
#endif

#define UXENDISP_PORT 0xd1580
#define UXENDISP_ALT_PORT 0xd1581
#define UXENDISP_VBLANK_PORT 0xd1582
#define UXENDISP_RING_SIZE 4096
#define UXENDISP_MAX_MSG_LEN 1024

#if defined(_MSC_VER)
#define UXENDISP_PACKED
#pragma pack(push, 1)
#pragma warning(push)
#else
#define UXENDISP_PACKED __attribute__((packed))
#endif

#define DISP_INVALID_RECT_ID           0xFFFFFFFFFFFFFFFFULL

#define DISP_COMPOSE_RECT_MAX                  32
#define DISP_COMPOSE_MODE_NONE                 0x0
#define DISP_COMPOSE_MODE_OVERLAY_DWM_RECTS    0x1

/* frontend -> backend */
struct dirty_rect_msg {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
    uint64_t rect_id;
} UXENDISP_PACKED;

/* backend -> frontend */
struct update_msg {
    /* last processed dirty rectangle id */
    uint64_t rect_done;
} UXENDISP_PACKED;

/* Escape code: GDI->display driver */
enum {
    UXENDISP_ESCAPE_SET_CUSTOM_MODE = 0x10001,
    UXENDISP_ESCAPE_SET_VIRTUAL_MODE = 0x10002,
    UXENDISP_ESCAPE_IS_VIRT_MODE_ENABLED = 0x10003,
    UXENDISP_ESCAPE_MAP_FB = 0x10004,
    UXENDISP_ESCAPE_UNMAP_FB = 0x10005,
    UXENDISP_ESCAPE_UPDATE_RECT = 0x10006,
    UXENDISP_ESCAPE_SET_USER_DRAW_ONLY = 0x10007,
    UXENDISP_ESCAPE_SET_NO_PRESENT_COPY = 0x10008,
    UXENDISP_ESCAPE_FLUSH = 0x10009,
    UXENDISP_ESCAPE_GET_USER_DRAW_ONLY = 0x1000a,
    UXENDISP_ESCAPE_GET_NO_PRESENT_COPY = 0x1000b,
    UXENDISP_ESCAPE_MAP_SCRATCH_FB = 0x1000c,
    UXENDISP_ESCAPE_UNMAP_SCRATCH_FB = 0x1000d,
    UXENDISP_ESCAPE_SCRATCHIFY_PROCESS = 0x1000e,
    UXENDISP_ESCAPE_UNSCRATCHIFY_PROCESS = 0x1000f,
    UXENDISP_ESCAPE_UPDATE_COMPOSED_RECTS = 0x10010,
    UXENDISP_ESCAPE_SET_COMPOSE_MODE = 0x10011,
};

struct _UXENDISPComposedRect {
    uint32_t x, y, w, h;
} UXENDISP_PACKED;

typedef struct _UXENDISPComposedRect UXENDISPComposedRect;

struct _UXENDISPCustomMode {
    int32_t esc_code;
    uint32_t width;
    uint32_t height;
    uint32_t x, y;

    union {
        uint64_t param;
        int user_draw;
        int no_present_copy;
        uint64_t ptr;
        uint32_t count;
    } UXENDISP_PACKED;
    /* bpp ? */

    /* composed rectangle data might follow */
} UXENDISP_PACKED;

typedef struct _UXENDISPCustomMode UXENDISPCustomMode;

#undef UXENDISP_PACKED
#if defined(_MSC_VER)
#pragma warning(pop)
#pragma pack(pop)
#endif

#endif // _DISP_H_
