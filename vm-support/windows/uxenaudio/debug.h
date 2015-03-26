/********************************************************************************
**    Copyright (c) 1998-1999 Microsoft Corporation. All Rights Reserved.
**
**       Portions Copyright (c) 1998-1999 Intel Corporation
**
********************************************************************************/
/*
 * uXen changes:
 *
 * Copyright 2013-2015, Bromium, Inc.
 * SPDX-License-Identifier: ISC
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef _DEBUG_H_
#define _DEBUG_H_

#include "../common/debug.h"

//
// Modified version of ksdebug.h to support runtime debug level changes.
//
const int DBG_NONE     = 0x00000000;
const int DBG_PRINT    = 0x00000001; // Blabla. Function entries for example
const int DBG_WARNING  = 0x00000002; // warning level
const int DBG_ERROR    = 0x00000004; // this doesn't generate a breakpoint

// specific debug output; you don't have to enable DBG_PRINT for this.
const int DBG_STREAM   = 0x00000010; // Enables stream output.
const int DBG_POWER    = 0x00000020; // Enables power management output.
const int DBG_DMA      = 0x00000040; // Enables DMA engine output.
const int DBG_REGS     = 0x00000080; // Enables register outout.
const int DBG_PROBE    = 0x00000100; // Enables hardware probing output.
const int DBG_SYSINFO  = 0x00000200; // Enables system info output.
const int DBG_VSR      = 0x00000400; // Enables variable sample rate output.
const int DBG_PROPERTY = 0x00000800; // Enables property handler output
const int DBG_POSITION = 0x00001000; // Enables printing of position on GetPosition
const int DBG_PINS     = 0x10000000; // Enables dump of created pins in topology
const int DBG_NODES    = 0x20000000; // Enables dump of created nodes in topology
const int DBG_CONNS    = 0x40000000; // Enables dump of the connections in topology
                                    
const int DBG_ALL      = 0xFFFFFFFF;

//
// The default statements that will print are warnings (DBG_WARNING) and
// errors (DBG_ERROR).
//
#if 0
const int DBG_DEFAULT = 0x00000004;  // Errors only.
#else
const int DBG_DEFAULT = 0x7fffffff;  // Errors only.
#endif

    
//
// Define global debug variable.
//
#ifdef DEFINE_DEBUG_VARS
#if (DBG)
unsigned long ulDebugOut = DBG_DEFAULT;
#endif

#else // !DEFINED_DEBUG_VARS
#if (DBG)
extern unsigned long ulDebugOut;
#endif
#endif


//
// Define the print statement.
//
#if defined(__cplusplus)
extern "C" {
#endif // #if defined(__cplusplus)

//
// DBG is 1 in checked builds
//
#if (DBG)
#define DOUT(lvl, strings)          \
    if ((lvl) & ulDebugOut)         \
    {                               \
        DbgPrint(STR_MODULENAME);   \
        DbgPrint##strings;          \
        DbgPrint("\n");             \
    }

#define BREAK()                     \
    DbgBreakPoint()

#else // if (!DBG)
#define DOUT(lvl, strings)
#define BREAK()
#endif // !DBG    

#if 0
#define FISH 0
#else
#pragma warning( disable : 4127 )
#define FISH do { DbgPrint("%s:%s:%d\n",__FILE__,__FUNCTION__,__LINE__); } while (0)
#endif

#if defined(__cplusplus)
}
#endif // #if defined(__cplusplus)


VOID uxenaudio_debug(const CHAR *fmt, ...);

#endif

