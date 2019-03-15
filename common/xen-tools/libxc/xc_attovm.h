/*
 * Copyright 2019, Bromium, Inc.
 * Author: Tomasz Wroblewski <tomasz.wroblewski@bromium.com>
 * SPDX-License-Identifier: ISC
 */

#ifndef XC_ATTOVM_H_
#define XC_ATTOVM_H_

/* in attoxen-api/ax_attovm.h */
struct attovm_definition_v1;

int xc_attovm_build(xc_interface *xch,
  uint32_t domid,
  uint32_t nr_vcpus,
  uint32_t memsize_mb,
  const char *image_filename,
  const char *appdef,
  uint32_t appdef_len,
  int seal);

int xc_attovm_image_create_from_live_vm(
  xc_interface *xch,
  uint32_t domid,
  uint32_t nr_vcpus,
  uint32_t memsize_mb,
  const char *filename);

int xc_attovm_change_focus(xc_interface *xch, uint32_t domid, uint32_t offer_focus);
#endif

