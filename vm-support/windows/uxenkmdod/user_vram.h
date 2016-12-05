/*
 * Copyright 2016, Bromium, Inc.
 * Author: Kris Uchronski <kris@bromium.com>
 * SPDX-License-Identifier: ISC
 */

#ifndef __USER_VRAM_H__
#define __USER_VRAM_H__

PMDL user_vram_init(PHYSICAL_ADDRESS vram_start, SIZE_T vram_size);

PVOID user_vram_map(PMDL vram_mdl);

#endif //__USER_VRAM_H__
