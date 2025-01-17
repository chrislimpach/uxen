/*
 * Copyright (c) 1998-2000 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.1 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 * 
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
/*
 * uXen changes:
 *
 * Copyright 2013-2015, Bromium, Inc.
 * Author: Julian Pidancet <julian@pidancet.net>
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

#ifndef _APPLEPS2TOADBMAP_H
#define _APPLEPS2TOADBMAP_H

#define DEADKEY                 0x80
#define ADB_CONVERTER_LEN       256 * 2     // 0x00~0xff : normal key , 0x100~0x1ff : extended key
#define ADB_CONVERTER_EX_START  256

// PS/2 scancode reference : USB HID to PS/2 Scan Code Translation Table PS/2 Set 1 columns
// http://download.microsoft.com/download/1/6/1/161ba512-40e2-4cc9-843a-923143f3456c/translate.pdf
static const UInt8 PS2ToADBMap[ADB_CONVERTER_LEN] = 
{
/*  ADB        AT  ANSI Key-Legend
    ======================== */
    DEADKEY,// 00
    0x35,   // 01  Escape
    0x12,   // 02  1!
    0x13,   // 03  2@
    0x14,   // 04  3#
    0x15,   // 05  4$
    0x17,   // 06  5%
    0x16,   // 07  6^
    0x1a,   // 08  7&
    0x1c,   // 09  8*
    0x19,   // 0a  9(
    0x1d,   // 0b  0)
    0x1b,   // 0c  -_
    0x18,   // 0d  =+
    0x33,   // 0e  Backspace
    0x30,   // 0f  Tab
    0x0c,   // 10  qQ
    0x0d,   // 11  wW
    0x0e,   // 12  eE
    0x0f,   // 13  rR
    0x11,   // 14  tT
    0x10,   // 15  yY
    0x20,   // 16  uU
    0x22,   // 17  iI
    0x1f,   // 18  oO
    0x23,   // 19  pP
    0x21,   // 1a  [{
    0x1e,   // 1b  ]}
    0x24,   // 1c  Return
    0x3b,   // 1d  Left Control
    0x00,   // 1e  aA
    0x01,   // 1f  sS
    0x02,   // 20  dD
    0x03,   // 21  fF
    0x05,   // 22  gG
    0x04,   // 23  hH
    0x26,   // 24  jJ
    0x28,   // 25  kK
    0x25,   // 26  lL
    0x29,   // 27  ;:
    0x27,   // 28  '"
    0x32,   // 29  `~
    0x38,   // 2a  Left Shift
    0x2a,   // 2b  \| , Europe 1(ISO)
    0x06,   // 2c  zZ
    0x07,   // 2d  xX
    0x08,   // 2e  cC
    0x09,   // 2f  vV
    0x0b,   // 30  bB
    0x2d,   // 31  nN
    0x2e,   // 32  mM
    0x2b,   // 33  ,<
    0x2f,   // 34  .>
    0x2c,   // 35  /?
    0x3c,   // 36  Right Shift
    0x43,   // 37  Keypad *
    0x3a,   // 38  Left Alt
    0x31,   // 39  Space
    0x39,   // 3a  Caps Lock
    0x7a,   // 3b  F1
    0x78,   // 3c  F2
    0x63,   // 3d  F3
    0x76,   // 3e  F4
    0x60,   // 3f  F5
    0x61,   // 40  F6
    0x62,   // 41  F7
    0x64,   // 42  F8
    0x65,   // 43  F9
    0x6d,   // 44  F10
    0x47,   // 45  Num Lock
    0x6b,   // 46  Scroll Lock
    0x59,   // 47  Keypad 7 Home
    0x5b,   // 48  Keypad 8 Up
    0x5c,   // 49  Keypad 9 PageUp
    0x4e,   // 4a  Keypad -
    0x56,   // 4b  Keypad 4 Left
    0x57,   // 4c  Keypad 5
    0x58,   // 4d  Keypad 6 Right
    0x45,   // 4e  Keypad +
    0x53,   // 4f  Keypad 1 End
    0x54,   // 50  Keypad 2 Down
    0x55,   // 51  Keypad 3 PageDn
    0x52,   // 52  Keypad 0 Insert
    0x41,   // 53  Keypad . Delete
    0x44,   // 54  SysReq
    0x46,   // 55
    0x0a,   // 56  Europe 2(ISO)
    0x67,   // 57  F11
    0x6f,   // 58  F12
    0x51,   // 59  Keypad =
    DEADKEY,// 5a
    DEADKEY,// 5b
    0x5f,   // 5c  Keyboard Int'l 6 (PC9800 Keypad , )
    DEADKEY,// 5d
    DEADKEY,// 5e
    DEADKEY,// 5f
    DEADKEY,// 60
    DEADKEY,// 61
    DEADKEY,// 62
    DEADKEY,// 63
    0x69,   // 64  F13
    0x6b,   // 65  F14
    0x71,   // 66  F15
    0x6a,   // 67  F16
    0x40,   // 68  F17
    0x4f,   // 69  F18
    0x50,   // 6a  F19
    0x5a,   // 6b  F20
    DEADKEY,// 6c  F21
    DEADKEY,// 6d  F22
    DEADKEY,// 6e  F23
    DEADKEY,// 6f
    0x68,   // 70  Keyboard Intl'2 (Japanese Katakana/Hiragana)
    DEADKEY,// 71
    DEADKEY,// 72
    0x5e,   // 73  Keyboard Int'l 1 (Japanese Ro)
    DEADKEY,// 74
    DEADKEY,// 75
    DEADKEY,// 76  F24 , Keyboard Lang 5 (Japanese Zenkaku/Hankaku)
    0x68,   // 77  Keyboard Lang 4 (Japanese Hiragana)
    0x68,   // 78  Keyboard Lang 3 (Japanese Katakana)
    0x68,   // 79  Keyboard Int'l 4 (Japanese Henkan)
    DEADKEY,// 7a
    0x66,   // 7b  Keyboard Int'l 5 (Japanese Muhenkan)
    DEADKEY,// 7c
    0x5d,   // 7d  Keyboard Int'l 3 (Japanese Yen)
    0x5f,   // 7e  Keypad , (Brazilian Keypad .)
    DEADKEY,// 7f 
    DEADKEY,// 80 
    DEADKEY,// 81 
    DEADKEY,// 82 
    DEADKEY,// 83 
    DEADKEY,// 84 
    DEADKEY,// 85 
    DEADKEY,// 86 
    DEADKEY,// 87 
    DEADKEY,// 88 
    DEADKEY,// 89 
    DEADKEY,// 8a 
    DEADKEY,// 8b 
    DEADKEY,// 8c 
    DEADKEY,// 8d 
    DEADKEY,// 8e 
    DEADKEY,// 8f 
    DEADKEY,// 90 
    DEADKEY,// 91 
    DEADKEY,// 92 
    DEADKEY,// 93 
    DEADKEY,// 94 
    DEADKEY,// 95 
    DEADKEY,// 96 
    DEADKEY,// 97 
    DEADKEY,// 98 
    DEADKEY,// 99 
    DEADKEY,// 9a 
    DEADKEY,// 9b 
    DEADKEY,// 9c 
    DEADKEY,// 9d 
    DEADKEY,// 9e 
    DEADKEY,// 9f 
    DEADKEY,// a0 
    DEADKEY,// a1 
    DEADKEY,// a2 
    DEADKEY,// a3 
    DEADKEY,// a4 
    DEADKEY,// a5 
    DEADKEY,// a6 
    DEADKEY,// a7 
    DEADKEY,// a8 
    DEADKEY,// a9 
    DEADKEY,// aa 
    DEADKEY,// ab 
    DEADKEY,// ac 
    DEADKEY,// ad 
    DEADKEY,// ae 
    DEADKEY,// af 
    DEADKEY,// b0 
    DEADKEY,// b1 
    DEADKEY,// b2 
    DEADKEY,// b3 
    DEADKEY,// b4 
    DEADKEY,// b5 
    DEADKEY,// b6 
    DEADKEY,// b7 
    DEADKEY,// b8 
    DEADKEY,// b9 
    DEADKEY,// ba 
    DEADKEY,// bb 
    DEADKEY,// bc 
    DEADKEY,// bd 
    DEADKEY,// be 
    DEADKEY,// bf 
    DEADKEY,// c0 
    DEADKEY,// c1 
    DEADKEY,// c2 
    DEADKEY,// c3 
    DEADKEY,// c4 
    DEADKEY,// c5 
    DEADKEY,// c6 
    DEADKEY,// c7 
    DEADKEY,// c8 
    DEADKEY,// c9 
    DEADKEY,// ca 
    DEADKEY,// cb 
    DEADKEY,// cc 
    DEADKEY,// cd 
    DEADKEY,// ce 
    DEADKEY,// cf 
    DEADKEY,// d0 
    DEADKEY,// d1 
    DEADKEY,// d2 
    DEADKEY,// d3 
    DEADKEY,// d4 
    DEADKEY,// d5 
    DEADKEY,// d6 
    DEADKEY,// d7 
    DEADKEY,// d8 
    DEADKEY,// d9 
    DEADKEY,// da 
    DEADKEY,// db 
    DEADKEY,// dc 
    DEADKEY,// dd 
    DEADKEY,// de 
    DEADKEY,// df 
    DEADKEY,// e0 
    DEADKEY,// e1 
    DEADKEY,// e2 
    DEADKEY,// e3 
    DEADKEY,// e4 
    DEADKEY,// e5 
    DEADKEY,// e6 
    DEADKEY,// e7 
    DEADKEY,// e8 
    DEADKEY,// e9 
    DEADKEY,// ea 
    DEADKEY,// eb 
    DEADKEY,// ec 
    DEADKEY,// ed 
    DEADKEY,// ee 
    DEADKEY,// ef 
    DEADKEY,// f0 
    0x66,   // f1*  Keyboard Lang 2 (Korean Hanja)
    0x68,   // f2*  Keyboard Lang 1 (Korean Hangul)
    DEADKEY,// f3 
    DEADKEY,// f4 
    DEADKEY,// f5 
    DEADKEY,// f6 
    DEADKEY,// f7 
    DEADKEY,// f8 
    DEADKEY,// f9 
    DEADKEY,// fa 
    DEADKEY,// fb 
    DEADKEY,// fc 
    DEADKEY,// fd 
    DEADKEY,// fe 
    DEADKEY,// ff 
    DEADKEY,// e0 00 
    DEADKEY,// e0 01 
    DEADKEY,// e0 02 
    DEADKEY,// e0 03 
    DEADKEY,// e0 04 
    0x91,   // e0 05 dell down
    0x90,   // e0 06 dell up
    DEADKEY,// e0 07
#ifndef PROBOOK
    0x90,   // e0 08 samsung up
    0x91,   // e0 09 samsung down
#else
    DEADKEY,// e0 08
    0x83,   // e0 09 Launchpad (hp Fn+F6)
#endif
    0xa0,   // e0 0a Mission Control (hp Fn+F5)
    DEADKEY,// e0 0b 
    DEADKEY,// e0 0c 
    DEADKEY,// e0 0d 
    DEADKEY,// e0 0e 
    DEADKEY,// e0 0f 
    0x4d,   // e0 10  Scan Previous Track (hp Fn+F10)
    DEADKEY,// e0 11 
    0x91,   // e0 12 hp down (Fn+F2)
    DEADKEY,// e0 13 
    DEADKEY,// e0 14 
    DEADKEY,// e0 15 
    DEADKEY,// e0 16 
    0x90,   // e0 17 hp up (Fn+F3)
    DEADKEY,// e0 18 
    0x42,   // e0 19  Scan Next Track (hp Fn+F12)
    DEADKEY,// e0 1a 
    DEADKEY,// e0 1b 
    0x4c,   // e0 1c  Keypad Enter
    0x3e,   // e0 1d  Right Control
    DEADKEY,// e0 1e 
    DEADKEY,// e0 1f 
    0x4a,   // e0 20  Mute (hp Fn+F7)
    DEADKEY,// e0 21  Calculator
    0x34,   // e0 22  Play/Pause (hp Fn+F11)
    DEADKEY,// e0 23 
    DEADKEY,// e0 24  Stop
    DEADKEY,// e0 25 
    DEADKEY,// e0 26 
    DEADKEY,// e0 27 
    DEADKEY,// e0 28 
    DEADKEY,// e0 29 
    DEADKEY,// e0 2a 
    DEADKEY,// e0 2b 
    DEADKEY,// e0 2c 
    DEADKEY,// e0 2d 
    0x49,   // e0 2e  Volume Down (hp Fn+F8)
    DEADKEY,// e0 2f 
    0x48,   // e0 30  Volume Up (hp Fn+F9)
    DEADKEY,// e0 31 
    DEADKEY,// e0 32  WWW Home
    DEADKEY,// e0 33 
    DEADKEY,// e0 34 
    0x4b,   // e0 35  Keypad /
    DEADKEY,// e0 36 
    0x69,   // e0 37  Print Screen
    0x3d,   // e0 38  Right Alt
    DEADKEY,// e0 39 
    DEADKEY,// e0 3a 
    DEADKEY,// e0 3b 
    DEADKEY,// e0 3c 
    DEADKEY,// e0 3d 
    DEADKEY,// e0 3e 
    DEADKEY,// e0 3f 
    DEADKEY,// e0 40 
    DEADKEY,// e0 41 
    DEADKEY,// e0 42 
    DEADKEY,// e0 43 
    DEADKEY,// e0 44 
    0x71,   // e0 45* Pause
    DEADKEY,// e0 46* Break(Ctrl-Pause)
    0x73,   // e0 47  Home
    0x7e,   // e0 48  Up Arrow
    0x74,   // e0 49  Page Up 
    DEADKEY,// e0 4a 
    0x7b,   // e0 4b  Left Arrow
    DEADKEY,// e0 4c 
    0x7c,   // e0 4d  Right Arrow
    0x90,   // e0 4e acer up
    0x77,   // e0 4f  End
    0x7d,   // e0 50  Down Arrow
    0x79,   // e0 51  Page Down
    0x72,   // e0 52  Insert
    0x75,   // e0 53  Delete
    DEADKEY,// e0 54 
    DEADKEY,// e0 55 
    DEADKEY,// e0 56 
    DEADKEY,// e0 57 
    DEADKEY,// e0 58 
    0x90,   // e0 59 acer up for my acer
    DEADKEY,// e0 5a 
    0x37,   // e0 5b  Left GUI(Windows)
    0x36,   // e0 5c  Right GUI(Windows)
    0x6e,   // e0 5d  App( Windows context menu key )
    0x7f,   // e0 5e  System Power / Keyboard Power
    DEADKEY,// e0 5f  System Sleep (hp Fn+F1)
    DEADKEY,// e0 60 
    DEADKEY,// e0 61 
    DEADKEY,// e0 62 
    DEADKEY,// e0 63  System Wake
    DEADKEY,// e0 64 
    DEADKEY,// e0 65  WWW Search
    DEADKEY,// e0 66  WWW Favorites
    DEADKEY,// e0 67  WWW Refresh
    DEADKEY,// e0 68  WWW Stop
    DEADKEY,// e0 69  WWW Forward
    DEADKEY,// e0 6a  WWW Back
    DEADKEY,// e0 6b  My Computer
    DEADKEY,// e0 6c  Mail
    DEADKEY,// e0 6d  Media Select
#ifndef PROBOOK
    0x90,   // e0 6e acer up
    0x91,   // e0 6f acer down
#else
    0x70,   // e0 6e  Video Mirror = hp Fn+F4
    DEADKEY,// e0 6f  Fn+Home
#endif
    DEADKEY,// e0 70
    DEADKEY,// e0 71 
    DEADKEY,// e0 72 
    DEADKEY,// e0 73 
    DEADKEY,// e0 74 
    DEADKEY,// e0 75 
    DEADKEY,// e0 76
#ifndef PROBOOK
    0x91,   // e0 77 lg down
    0x90,   // e0 78 lg up
#else
    DEADKEY,// e0 77
    DEADKEY,// e0 78 WiFi on/off button on HP ProBook
#endif
    DEADKEY,// e0 79
    DEADKEY,// e0 7a 
    DEADKEY,// e0 7b 
    DEADKEY,// e0 7c 
    DEADKEY,// e0 7d 
    DEADKEY,// e0 7e 
    DEADKEY,// e0 7f 
    DEADKEY,// e0 80 
    DEADKEY,// e0 81 
    DEADKEY,// e0 82 
    DEADKEY,// e0 83 
    DEADKEY,// e0 84 
    DEADKEY,// e0 85 
    DEADKEY,// e0 86 
    DEADKEY,// e0 87 
    DEADKEY,// e0 88 
    DEADKEY,// e0 89 
    DEADKEY,// e0 8a 
    DEADKEY,// e0 8b 
    DEADKEY,// e0 8c 
    DEADKEY,// e0 8d 
    DEADKEY,// e0 8e 
    DEADKEY,// e0 8f 
    DEADKEY,// e0 90 
    DEADKEY,// e0 91 
    DEADKEY,// e0 92 
    DEADKEY,// e0 93 
    DEADKEY,// e0 94 
    DEADKEY,// e0 95 
    DEADKEY,// e0 96 
    DEADKEY,// e0 97 
    DEADKEY,// e0 98 
    DEADKEY,// e0 99 
    DEADKEY,// e0 9a 
    DEADKEY,// e0 9b 
    DEADKEY,// e0 9c 
    DEADKEY,// e0 9d 
    DEADKEY,// e0 9e 
    DEADKEY,// e0 9f 
    DEADKEY,// e0 a0 
    DEADKEY,// e0 a1 
    DEADKEY,// e0 a2 
    DEADKEY,// e0 a3 
    DEADKEY,// e0 a4 
    DEADKEY,// e0 a5 
    DEADKEY,// e0 a6 
    DEADKEY,// e0 a7 
    DEADKEY,// e0 a8 
    DEADKEY,// e0 a9 
    DEADKEY,// e0 aa 
    DEADKEY,// e0 ab 
    DEADKEY,// e0 ac 
    DEADKEY,// e0 ad 
    DEADKEY,// e0 ae 
    DEADKEY,// e0 af 
    DEADKEY,// e0 b0 
    DEADKEY,// e0 b1 
    DEADKEY,// e0 b2 
    DEADKEY,// e0 b3 
    DEADKEY,// e0 b4 
    DEADKEY,// e0 b5 
    DEADKEY,// e0 b6 
    DEADKEY,// e0 b7 
    DEADKEY,// e0 b8 
    DEADKEY,// e0 b9 
    DEADKEY,// e0 ba 
    DEADKEY,// e0 bb 
    DEADKEY,// e0 bc 
    DEADKEY,// e0 bd 
    DEADKEY,// e0 be 
    DEADKEY,// e0 bf 
    DEADKEY,// e0 c0 
    DEADKEY,// e0 c1 
    DEADKEY,// e0 c2 
    DEADKEY,// e0 c3 
    DEADKEY,// e0 c4 
    DEADKEY,// e0 c5 
    DEADKEY,// e0 c6 
    DEADKEY,// e0 c7 
    DEADKEY,// e0 c8 
    DEADKEY,// e0 c9 
    DEADKEY,// e0 ca 
    DEADKEY,// e0 cb 
    DEADKEY,// e0 cc 
    DEADKEY,// e0 cd 
    DEADKEY,// e0 ce 
    DEADKEY,// e0 cf 
    DEADKEY,// e0 d0 
    DEADKEY,// e0 d1 
    DEADKEY,// e0 d2 
    DEADKEY,// e0 d3 
    DEADKEY,// e0 d4 
    DEADKEY,// e0 d5 
    DEADKEY,// e0 d6 
    DEADKEY,// e0 d7 
    DEADKEY,// e0 d8 
    DEADKEY,// e0 d9 
    DEADKEY,// e0 da 
    DEADKEY,// e0 db 
    DEADKEY,// e0 dc 
    DEADKEY,// e0 dd 
    DEADKEY,// e0 de 
    DEADKEY,// e0 df 
    DEADKEY,// e0 e0 
    DEADKEY,// e0 e1 
    DEADKEY,// e0 e2 
    DEADKEY,// e0 e3 
    DEADKEY,// e0 e4 
    DEADKEY,// e0 e5 
    DEADKEY,// e0 e6 
    DEADKEY,// e0 e7 
    DEADKEY,// e0 e8 
    DEADKEY,// e0 e9 
    DEADKEY,// e0 ea 
    DEADKEY,// e0 eb 
    DEADKEY,// e0 ec 
    DEADKEY,// e0 ed 
    DEADKEY,// e0 ee 
    DEADKEY,// e0 ef 
    DEADKEY,// e0 f0 
    DEADKEY,// e0 f1
    DEADKEY,// e0 f2
    DEADKEY,// e0 f3 
    DEADKEY,// e0 f4 
    DEADKEY,// e0 f5 
    DEADKEY,// e0 f6 
    DEADKEY,// e0 f7 
    DEADKEY,// e0 f8 
    DEADKEY,// e0 f9 
    DEADKEY,// e0 fa 
    DEADKEY,// e0 fb 
    DEADKEY,// e0 fc 
    DEADKEY,// e0 fd 
    DEADKEY,// e0 fe 
    DEADKEY // e0 ff 
};

#endif /* !_APPLEPS2TOADBMAP_H */
