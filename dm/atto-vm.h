/*
 * Copyright 2019, Bromium, Inc.
 * Author: Tomasz Wroblewski <tomasz.wroblewski@bromium.com>
 * SPDX-License-Identifier: ISC
 */

#ifndef _ATTO_VM_H_
#define _ATTO_VM_H_

struct display_state;

void attovm_set_current_cursor(struct display_state* ds);
void attovm_check_kbd_layout_change(void);
void attovm_set_x11_cursor(struct display_state* ds, uint64_t x11_ptr);
void attovm_map_x11_cursor(int x11_type, uint64_t x11_ptr);
void attovm_unmap_x11_cursor(uint64_t x11_ptr);
void attovm_create_custom_cursor(uint64_t x11_ptr, int xhot, int yhot,
                                 int x11_nx, int x11_ny,
                                 int nbytes, uint8_t *data);
void attovm_paint_splash(HWND hwnd, HBITMAP splash_bitmap);
void attovm_set_keyboard_focus(int offer_focus);
void attovm_check_keyboard_focus(void);
int is_attovm_image(const char *file);

#endif
