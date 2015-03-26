/*
 * Copyright 2013-2015, Bromium, Inc.
 * Author: Kris Uchronski <kris@bromium.com>
 * SPDX-License-Identifier: ISC
 */

#include "kdx.h"

def_usym_sizeof (domain, 0x1000);
def_usym        (domain, domain_id,         0x0000);
def_usym_addr   (domain, page_list_next,    0x0030);
def_usym_addr   (domain, page_list_tail,    0x0038);
def_usym        (domain, max_vcpus,         0x008c);
def_usym_addr   (domain, next_in_list,      0x00a0);
def_usym_addr   (domain, vcpu,              0x0278);

def_usym_sizeof (vcpu, 0x1000);
def_usym        (vcpu, vcpu_id,             0x0000);
def_usym        (vcpu, is_running,          0x020b);
def_usym        (vcpu, arch_hvm_vcpu_u_vmx_vmcs,        0x0680);
def_usym        (vcpu, arch_hvm_vcpu_u_vmx_vmcs_ma,     0x0688);
def_usym        (vcpu, arch_hvm_vcpu_u_vmx_vmcs_shadow, 0x0690);
def_usym        (vcpu, arch_hvm_vcpu_u_vmx_active_cpu,  0x06b8);
def_usym        (vcpu, arch_hvm_vcpu_u_vmx_launched,    0x06bc);

EXT_COMMAND(
    domain,
    "displays various information about uxen domains",
    "{;ed,o;expr;domain address}"
    "{v;b;;show page details}"
    "{b;b;;dump pages backwards}"
    "{d;ed;;show given first number of bytes}")
{
    RequireKernelMode();

    ULONG64 usym_addr(domain);

    if (HasUnnamedArg(0)) {
        /* dump specific domain details */
        ULONG64 frametable_addr = GetExpression("poi(uxen!frametable)");
        usym_addr(domain) = GetUnnamedArgU64(0);
        usym_fetch_struct(domain, return);
        usym_def_addr(domain, page_list_next);
        usym_def_addr(domain, page_list_tail);

        Out("[domain @ 0x%p, id:%hd]\n"
            "  frametable:0x%p\n"
            "  page_list_next:0x%p, page_list_tail:0x%p\n",
            usym_addr(domain), usym_read_u16(domain, domain_id),
            frametable_addr,
            usym_addr(domain_page_list_next),
            usym_addr(domain_page_list_tail));

        dump_page_list(HasArg("db") ? usym_addr(domain_page_list_tail) :
                                      usym_addr(domain_page_list_next),
                       frametable_addr, HasArg("b"), HasArg("v"),
                       HasArg("d") ? GetArgU64("d", false) : 0);
    } else {
        /* domain address not provided - dump domain list */
        usym_addr(domain) = GetExpression("poi(uxen!domain_list)");
        while (0 != usym_addr(domain)) {
            usym_fetch_struct(domain, break);
            usym_def_u32(domain, max_vcpus);
            usym_def_addr(domain, vcpu);

            Dml("[<exec cmd=\"!domain 0x%p\">domain @ 0x%p</exec>] domain_id:%hd, max_vcpus:%d, vcpu:0x%p\n",
                usym_addr(domain), usym_addr(domain),
                usym_read_u16(domain, domain_id),
                domain_max_vcpus, usym_addr(domain_vcpu));

            usym_fetch_array(domain_vcpu, domain_max_vcpus * VM_PTR_SIZE,
                             VM_PTR_TYPE, goto next_domain);

            for (ULONG i = 0; i < domain_max_vcpus; i++) {
                VM_PTR_TYPE usym_addr(vcpu) = usym_arr(domain_vcpu)[i];
                usym_fetch_struct(vcpu, continue);

                Dml("    vcpu[%d]:0x%p, vcpu_id:%d, "
                    "is_running:%d, active_cpu:0x%x, launched:0x%x, "
                    "vmcs:0x%p, vmcs_ma:0x%p, vmcs_shadow:0x%p\n",
                    i, usym_addr(vcpu), usym_read_u32(vcpu, vcpu_id),
                    usym_read_u8(vcpu, is_running),
                    usym_read_u32(vcpu, arch_hvm_vcpu_u_vmx_active_cpu),
                    usym_read_u32(vcpu, arch_hvm_vcpu_u_vmx_launched),
                    usym_read_u64(vcpu, arch_hvm_vcpu_u_vmx_vmcs),
                    usym_read_u64(vcpu, arch_hvm_vcpu_u_vmx_vmcs_ma),
                    usym_read_u64(vcpu, arch_hvm_vcpu_u_vmx_vmcs_shadow));
            }

            usym_free_arr(domain_vcpu);
            
          next_domain:
            usym_addr(domain) = usym_read_addr(domain, next_in_list);               
        }
    }
}
