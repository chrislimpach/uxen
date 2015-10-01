/*
 * Copyright 2015, Bromium, Inc.
 * Author: Julian Pidancet <julian@pidancet.net>
 * SPDX-License-Identifier: ISC
 */

#ifndef _UXENHID_COMMON_H_
#define _UXENHID_COMMON_H_


#if !defined(__KERNEL__)
#if !defined(_MSC_VER)
#include <stdint.h>
#else
typedef __int8 int8_t;
typedef __int16 int16_t;
typedef __int32 int32_t;
typedef __int64 int64_t;
typedef unsigned __int8 uint8_t;
typedef unsigned __int16 uint16_t;
typedef unsigned __int32 uint32_t;
typedef unsigned __int64 uint64_t;
#endif
#endif


#define UXENHID_PORT 0xe0000
#define UXENHID_RING_SIZE 65536
#define UXENHID_MAX_MSG_LEN 1024

typedef enum _UXENHID_MSG_TYPE {
    UXENHID_DEVICE_START                = 0x00000000,
    UXENHID_DEVICE_STOP                 = 0x00000001,
    UXENHID_REQUEST_REPORT_DESCRIPTOR   = 0x00000002,
    UXENHID_REPORT                      = 0x00000003,
    UXENHID_FEATURE_QUERY               = 0x00000004,
    UXENHID_FEATURE_REPORT              = 0x00000005,
    UXENHID_NOP                         = 0x00000006,
} UXENHID_MSG_TYPE;

enum {
    UXENHID_REPORT_ID_MOUSE = 1,
    UXENHID_REPORT_ID_PEN = 2,
    UXENHID_REPORT_ID_TOUCH = 3,
    UXENHID_REPORT_ID_MAX_CONTACT_COUNT = 4,
};

#define UXENHID_MOUSE_BUTTON_1  0x01
#define UXENHID_MOUSE_BUTTON_2  0x02
#define UXENHID_MOUSE_BUTTON_3  0x04
#define UXENHID_MOUSE_BUTTON_4  0x08
#define UXENHID_MOUSE_BUTTON_5  0x10

#define UXENHID_FLAG_IN_RANGE           0x01
#define UXENHID_FLAG_TIP_SWITCH         0x02
#define UXENHID_PEN_FLAG_BARREL_SWITCH  0x04
#define UXENHID_PEN_FLAG_INVERT         0x08
#define UXENHID_PEN_FLAG_ERASER         0x10

#define UXENHID_XY_MAX                  32767
#define UXENHID_WHEEL_MIN               -127
#define UXENHID_WHEEL_MAX               127
#define UXENHID_PRESSURE_MAX            1023
#define UXENHID_PHYS_X                  2540 /* 25.4 cm */
#define UXENHID_PHYS_Y                  1693 /* 16.93 cm */

#if defined(_MSC_VER)
#define UXENHID_PACKED
#pragma pack(push, 1)
#pragma warning(push)
#else
#define UXENHID_PACKED __attribute__((packed))
#endif

typedef struct _UXENHID_MSG_HEADER {
    uint32_t type;
    uint32_t msglen;
} UXENHID_PACKED UXENHID_MSG_HEADER, *PUXENHID_MSG_HEADER;

struct mouse_report
{
    uint8_t report_id;
    uint8_t buttons;
    uint16_t x;
    uint16_t y;
    int8_t wheel;
    int8_t hwheel;
} UXENHID_PACKED;

struct pen_report
{
    uint8_t report_id;
    uint8_t flags;
    uint16_t x;
    uint16_t y;
    uint16_t pressure;
} UXENHID_PACKED;

struct touch_report
{
    uint8_t report_id;
    uint8_t flags;
    uint16_t contact_id;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint8_t contact_count;
} UXENHID_PACKED;

struct max_contact_count_report
{
    uint8_t report_id;
    uint8_t max_contact_count;
} UXENHID_PACKED;

#undef UXENHID_PACKED
#if defined(_MSC_VER)
#pragma warning(pop)
#pragma pack(pop)
#endif

#endif /* _UXENHID_COMMON_H_ */
