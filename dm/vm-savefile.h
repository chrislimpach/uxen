/*
 * Copyright 2016, Bromium, Inc.
 * Author: Paulian Marinca <paulian@marinca.net>
 * SPDX-License-Identifier: ISC
 */

#ifndef _VMSAVEFILE_H_
#define _VMSAVEFILE_H_

#include "introspection_info.h"
#include <fingerprint.h>

#define SAVE_FORMAT_VERSION 4
// #include <xg_save_restore.h>
#define XC_SAVE_ID_VCPU_INFO          -2 /* Additional VCPU info */
#define XC_SAVE_ID_HVM_IDENT_PT       -3 /* (HVM-only) */
#define XC_SAVE_ID_HVM_VM86_TSS       -4 /* (HVM-only) */
#define XC_SAVE_ID_TSC_INFO           -7
#define XC_SAVE_ID_HVM_CONSOLE_PFN    -8 /* (HVM-only) */
#define XC_SAVE_ID_HVM_ACPI_IOPORTS_LOCATION -10
#define XC_SAVE_ID_HVM_MAGIC_PFNS     -11
#define XC_SAVE_ID_HVM_CONTEXT        -12
#define XC_SAVE_ID_HVM_DM             -13
#define XC_SAVE_ID_VM_UUID            -14
#define XC_SAVE_ID_VM_TEMPLATE_UUID   -15
#define XC_SAVE_ID_VERSION            -16
#define XC_SAVE_ID_HVM_INTROSPEC      -17
#define XC_SAVE_ID_MAPCACHE_PARAMS    -18
#define XC_SAVE_ID_VM_TEMPLATE_FILE   -19
#define XC_SAVE_ID_PAGE_OFFSETS       -20
#define XC_SAVE_ID_ZERO_BITMAP        -21
#define XC_SAVE_ID_FINGERPRINTS       -22
#define XC_SAVE_ID_CUCKOO_DATA        -23

struct xc_save_generic {
    int32_t marker;
    uint32_t size;
};

struct xc_save_version_info {
    int32_t marker;
    uint32_t version;
};

struct xc_save_tsc_info {
    int32_t marker;
    uint32_t tsc_mode;
    uint64_t nsec;
    uint32_t khz;
    uint32_t incarn;
};

struct xc_save_vcpu_info {
    int32_t marker;
    int max_vcpu_id;
    uint64_t vcpumap;
};

struct xc_save_hvm_generic_chunk {
    int32_t marker;
    uint32_t pad;
    uint64_t data;
};

struct xc_save_hvm_magic_pfns {
    int32_t marker;
    uint64_t magic_pfns[5];
};

struct xc_save_hvm_context {
    int32_t marker;
    uint32_t size;
    uint8_t context[];
};

struct xc_save_hvm_dm {
    int32_t marker;
    uint32_t size;
    uint8_t state[];
};

struct xc_save_vm_uuid {
    int32_t marker;
    uint8_t uuid[16];
};

struct xc_save_vm_template_uuid {
    int32_t marker;
    uint8_t uuid[16];
};

struct xc_save_hvm_introspec {
    int32_t marker;
    struct guest_introspect_info_header info;
};

struct xc_save_mapcache_params {
    int32_t marker;
    uint32_t end_low_pfn;
    uint32_t start_high_pfn;
    uint32_t end_high_pfn;
};

struct xc_save_vm_template_file {
    int32_t marker;
    uint16_t size;
    char file[];
};

struct xc_save_vm_page_offsets {
    struct xc_save_generic;

    uint32_t pfn_off_nr;
    uint64_t pfn_off[];
};

struct xc_save_zero_bitmap {
    struct xc_save_generic;

    uint32_t zero_bitmap_size;
    uint8_t data[];
};

struct xc_save_vm_fingerprints {
    struct xc_save_generic;

    uint32_t hashes_nr;
    struct page_fingerprint hashes[];
};

struct PACKED xc_save_index {
    uint64_t offset;
    int32_t marker;             /* marker field last such that the
                                 * regular end marker also doubles as
                                 * an index end marker */
};

struct xc_save_cuckoo_data {
    int32_t marker;
    int32_t simple_mode;
    uint8_t data[];
};

#endif
