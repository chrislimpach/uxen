/*
 * svm.c: handling SVM architecture-related VM exits
 * Copyright (c) 2004, Intel Corporation.
 * Copyright (c) 2005-2007, Advanced Micro Devices, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA 02111-1307 USA.
 */
/*
 * uXen changes:
 *
 * Copyright 2011-2019, Bromium, Inc.
 * Author: Christian Limpach <Christian.Limpach@gmail.com>
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

#include <xen/config.h>
#include <xen/init.h>
#include <xen/lib.h>
#include <xen/trace.h>
#include <xen/sched.h>
#include <xen/irq.h>
#include <xen/softirq.h>
#include <xen/hypercall.h>
#include <xen/domain_page.h>
#include <xen/xenoprof.h>
#include <asm/current.h>
#include <asm/io.h>
#include <asm/paging.h>
#include <asm/p2m.h>
#include <asm/regs.h>
#include <asm/cpufeature.h>
#include <asm/processor.h>
#include <asm/amd.h>
#include <asm/types.h>
#include <asm/debugreg.h>
#include <asm/msr.h>
#include <asm/i387.h>
#include <asm/spinlock.h>
#include <asm/hvm/emulate.h>
#include <asm/hvm/hvm.h>
#include <asm/hvm/support.h>
#include <asm/hvm/io.h>
#include <asm/hvm/emulate.h>
#include <asm/hvm/svm/asid.h>
#include <asm/hvm/svm/svm.h>
#include <asm/hvm/svm/vmcb.h>
#include <asm/hvm/svm/emulate.h>
#include <asm/hvm/svm/intr.h>
#include <asm/hvm/svm/svmdebug.h>
#include <asm/hvm/nestedhvm.h>
#include <asm/x86_emulate.h>
#include <public/sched.h>
#include <asm/hvm/vpt.h>
#include <asm/hvm/trace.h>
#include <asm/hap.h>
#include <asm/apic.h>
#include <asm/debugger.h>
#include <asm/xstate.h>
#include <asm/hvm/ax.h>

u32 svm_feature_flags;

/* Indicates whether guests may use EFER.LMSLE. */
bool_t cpu_has_lmsl;

#define set_segment_register(name, value)  \
    asm volatile ( "movw %%ax ,%%" STR(name) "" : : "a" (value) )

static struct hvm_function_table svm_function_table;

/* va of hardware host save area     */
static DEFINE_PER_CPU_READ_MOSTLY(void *, hsa);

/* vmcb used for extended host state */
static DEFINE_PER_CPU_READ_MOSTLY(void *, root_vmcb);

static DEFINE_PER_CPU(unsigned long, host_msr_tsc_aux);

static void svm_do_resume(struct vcpu *v);

static bool_t amd_erratum383_found __read_mostly;

void __update_guest_eip(struct cpu_user_regs *regs, unsigned int inst_len)
{
    struct vcpu *curr = current;

    if ( unlikely(inst_len == 0) )
        return;

    if ( unlikely(inst_len > 15) )
    {
        gdprintk(XENLOG_ERR, "Bad instruction length %u\n", inst_len);
        domain_crash(curr->domain);
        return;
    }

    ASSERT(regs == guest_cpu_user_regs());

    regs->eip += inst_len;
    regs->eflags &= ~X86_EFLAGS_RF;

    curr->arch.hvm_svm.vmcb->interrupt_shadow = 0;

    if ( regs->eflags & X86_EFLAGS_TF )
        hvm_inject_exception(TRAP_debug, HVM_DELIVER_NO_ERROR_CODE, 0);
}

unsigned long *
svm_msrbit(unsigned long *msr_bitmap, uint32_t msr)
{
    unsigned long *msr_bit = NULL;

    /*
     * See AMD64 Programmers Manual, Vol 2, Section 15.10 (MSR-Bitmap Address).
     */
    if ( msr <= 0x1fff )
        msr_bit = msr_bitmap + 0x0000 / BYTES_PER_LONG;
    else if ( (msr >= 0xc0000000) && (msr <= 0xc0001fff) )
        msr_bit = msr_bitmap + 0x0800 / BYTES_PER_LONG;
    else if ( (msr >= 0xc0010000) && (msr <= 0xc0011fff) )
        msr_bit = msr_bitmap + 0x1000 / BYTES_PER_LONG;

    return msr_bit;
}

void svm_intercept_msr(struct vcpu *v, uint32_t msr, int enable)
{
    unsigned long *msr_bit;

    msr_bit = svm_msrbit(v->arch.hvm_svm.msrpm, msr);
    BUG_ON(msr_bit == NULL);
    msr &= 0x1fff;

    if ( enable )
    {
        __set_bit(msr * 2, msr_bit);
        __set_bit(msr * 2 + 1, msr_bit);
    }
    else
    {
        __clear_bit(msr * 2, msr_bit);
        __clear_bit(msr * 2 + 1, msr_bit);
    }
}

#ifdef __UXEN_vdr__
static void svm_save_dr(struct vcpu *v)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;

    if ( !v->arch.hvm_vcpu.flag_dr_dirty )
        return;

    /* Clear the DR dirty flag and re-enable intercepts for DR accesses. */
    v->arch.hvm_vcpu.flag_dr_dirty = 0;
    vmcb_set_dr_intercepts(vmcb, ~0u);

    v->arch.debugreg[0] = read_debugreg(0);
    v->arch.debugreg[1] = read_debugreg(1);
    v->arch.debugreg[2] = read_debugreg(2);
    v->arch.debugreg[3] = read_debugreg(3);
    v->arch.debugreg[6] = vmcb_get_dr6(vmcb);
    v->arch.debugreg[7] = vmcb_get_dr7(vmcb);
}
#endif  /* __UXEN_vdr__ */

static void __restore_debug_registers(struct vcpu *v)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;

    if ( v->arch.hvm_vcpu.flag_dr_dirty )
        return;

    v->arch.hvm_vcpu.flag_dr_dirty = 1;
    vmcb_set_dr_intercepts(vmcb, 0);

    write_debugreg(0, v->arch.debugreg[0]);
    write_debugreg(1, v->arch.debugreg[1]);
    write_debugreg(2, v->arch.debugreg[2]);
    write_debugreg(3, v->arch.debugreg[3]);
    vmcb_set_dr6(vmcb, v->arch.debugreg[6]);
    vmcb_set_dr7(vmcb, v->arch.debugreg[7]);
}

#ifdef __UXEN_vdr__
/*
 * DR7 is saved and restored on every vmexit.  Other debug registers only
 * need to be restored if their value is going to affect execution -- i.e.,
 * if one of the breakpoints is enabled.  So mask out all bits that don't
 * enable some breakpoint functionality.
 */
static void svm_restore_dr(struct vcpu *v)
{
    if ( unlikely(v->arch.debugreg[7] & DR7_ACTIVE_MASK) )
        __restore_debug_registers(v);
}
#endif  /* __UXEN_vdr__ */

static int svm_vmcb_save(struct vcpu *v, struct hvm_hw_cpu *c)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;

    c->cr0 = v->arch.hvm_vcpu.guest_cr[0];
    c->cr2 = v->arch.hvm_vcpu.guest_cr[2];
    c->cr3 = v->arch.hvm_vcpu.guest_cr[3];
    c->cr4 = v->arch.hvm_vcpu.guest_cr[4];

    c->sysenter_cs = v->arch.hvm_svm.guest_sysenter_cs;
    c->sysenter_esp = v->arch.hvm_svm.guest_sysenter_esp;
    c->sysenter_eip = v->arch.hvm_svm.guest_sysenter_eip;

    c->pending_event = 0;
    c->error_code = 0;
    if ( vmcb->eventinj.fields.v &&
         hvm_event_needs_reinjection(vmcb->eventinj.fields.type,
                                     vmcb->eventinj.fields.vector) )
    {
        c->pending_event = (uint32_t)vmcb->eventinj.bytes;
        c->error_code = vmcb->eventinj.fields.errorcode;
    }

    return 1;
}

static int svm_vmcb_restore(struct vcpu *v, struct hvm_hw_cpu *c)
{
    unsigned long mfn = 0;
    p2m_type_t p2mt;
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;
    struct p2m_domain *p2m = p2m_get_hostp2m(v->domain);

    if ( c->pending_valid &&
         ((c->pending_type == 1) || (c->pending_type > 6) ||
          (c->pending_reserved != 0)) )
    {
        gdprintk(XENLOG_ERR, "Invalid pending event 0x%"PRIx32".\n",
                 c->pending_event);
        return -EINVAL;
    }

    if ( !paging_mode_hap(v->domain) )
    {
        if ( c->cr0 & X86_CR0_PG )
        {
            mfn = mfn_x(get_gfn(v->domain, c->cr3 >> PAGE_SHIFT, &p2mt));
            if ( !p2m_is_ram(p2mt) || !get_page(mfn_to_page(mfn), v->domain) )
            {
                put_gfn(v->domain, c->cr3 >> PAGE_SHIFT);
                gdprintk(XENLOG_ERR, "Invalid CR3 value=0x%"PRIx64"\n",
                         c->cr3);
                return -EINVAL;
            }
        }

        if ( v->arch.hvm_vcpu.guest_cr[0] & X86_CR0_PG )
            put_page(pagetable_get_page(v->arch.guest_table));

        v->arch.guest_table = pagetable_from_pfn(mfn);
        if ( c->cr0 & X86_CR0_PG )
            put_gfn(v->domain, c->cr3 >> PAGE_SHIFT);
    }

    v->arch.hvm_vcpu.guest_cr[0] = c->cr0 | X86_CR0_ET;
    v->arch.hvm_vcpu.guest_cr[2] = c->cr2;
    v->arch.hvm_vcpu.guest_cr[3] = c->cr3;
    v->arch.hvm_vcpu.guest_cr[4] = c->cr4;
    hvm_update_guest_cr(v, 0);
    hvm_update_guest_cr(v, 2);
    hvm_update_guest_cr(v, 4);

    /* Load sysenter MSRs into both VMCB save area and VCPU fields. */
    vmcb->sysenter_cs = v->arch.hvm_svm.guest_sysenter_cs = c->sysenter_cs;
    vmcb->sysenter_esp = v->arch.hvm_svm.guest_sysenter_esp = c->sysenter_esp;
    vmcb->sysenter_eip = v->arch.hvm_svm.guest_sysenter_eip = c->sysenter_eip;
    
    if ( paging_mode_hap(v->domain) )
    {
        vmcb_set_np_enable(vmcb, 1);
        vmcb_set_g_pat(vmcb, VMCB_DEFAULT_G_PAT);
        vmcb_set_h_cr3(vmcb, pagetable_get_paddr(p2m_get_pagetable(p2m)));
    }

    if ( c->pending_valid ) 
    {
        gdprintk(XENLOG_INFO, "Re-injecting 0x%"PRIx32", 0x%"PRIx32"\n",
                 c->pending_event, c->error_code);

        if ( hvm_event_needs_reinjection(c->pending_type, c->pending_vector) )
        {
            vmcb->eventinj.bytes = c->pending_event;
            vmcb->eventinj.fields.errorcode = c->error_code;
        }
    }

    vmcb->cleanbits.bytes = 0;
    paging_update_paging_modes(v);

    return 0;
}


static void svm_save_cpu_state(struct vcpu *v, struct hvm_hw_cpu *data)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;

    data->shadow_gs        = vmcb->kerngsbase;
    data->msr_lstar        = vmcb->lstar;
    data->msr_star         = vmcb->star;
    data->msr_cstar        = vmcb->cstar;
    data->msr_syscall_mask = vmcb->sfmask;
    data->msr_efer         = v->arch.hvm_vcpu.guest_efer;
    data->msr_flags        = -1ULL;

    /* must be done with paused time or tsc desyncs across vcpus */
    WARN_ON(!v->arch.pause_tsc);
    data->tsc = hvm_get_guest_tsc(v);
}


static void svm_load_cpu_state(struct vcpu *v, struct hvm_hw_cpu *data)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;

    vmcb->kerngsbase = data->shadow_gs;
    vmcb->lstar      = data->msr_lstar;
    vmcb->star       = data->msr_star;
    vmcb->cstar      = data->msr_cstar;
    vmcb->sfmask     = data->msr_syscall_mask;
    v->arch.hvm_vcpu.guest_efer = data->msr_efer;
    hvm_update_guest_efer(v);

    /* must be done with paused time or tsc desyncs across vcpus */
    WARN_ON(!v->arch.pause_tsc);
    hvm_set_guest_tsc(v, data->tsc);
}

void
svm_save_cpu_ctxt(struct vcpu *v, struct hvm_hw_cpu *ctxt)
{
    svm_save_cpu_state(v, ctxt);
    svm_vmcb_save(v, ctxt);
}

int
svm_load_cpu_ctxt(struct vcpu *v, struct hvm_hw_cpu *ctxt)
{
    int ret;

    svm_load_cpu_state(v, ctxt);
    ret = svm_vmcb_restore(v, ctxt);
    if (ret) {
        if (ret != -ECONTINUATION) {
            gdprintk(XENLOG_ERR, "svm_vmcb restore failed!\n");
            domain_crash(v->domain);
        }
        return ret;
    }

    return 0;
}

static void svm_fpu_enter(struct vcpu *v)
{
    struct vmcb_struct *n1vmcb = vcpu_nestedhvm(v).nv_n1vmcx;

    vmcb_set_exception_intercepts(
        n1vmcb,
        vmcb_get_exception_intercepts(n1vmcb) & ~(1U << TRAP_no_device));
}

unsigned int
svm_get_interrupt_shadow(struct vcpu *v)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;
    unsigned int intr_shadow = 0;

    if ( vmcb->interrupt_shadow )
        intr_shadow |= HVM_INTR_SHADOW_MOV_SS | HVM_INTR_SHADOW_STI;

    if ( vmcb_get_general1_intercepts(vmcb) & GENERAL1_INTERCEPT_IRET )
        intr_shadow |= HVM_INTR_SHADOW_NMI;

    return intr_shadow;
}

void
svm_set_interrupt_shadow(struct vcpu *v, unsigned int intr_shadow)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;
    u32 general1_intercepts = vmcb_get_general1_intercepts(vmcb);

    vmcb->interrupt_shadow =
        !!(intr_shadow & (HVM_INTR_SHADOW_MOV_SS|HVM_INTR_SHADOW_STI));

    general1_intercepts &= ~GENERAL1_INTERCEPT_IRET;
    if ( intr_shadow & HVM_INTR_SHADOW_NMI )
        general1_intercepts |= GENERAL1_INTERCEPT_IRET;
    vmcb_set_general1_intercepts(vmcb, general1_intercepts);
}

int
svm_guest_x86_mode(struct vcpu *v)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;

    if ( unlikely(!(v->arch.hvm_vcpu.guest_cr[0] & X86_CR0_PE)) )
        return 0;
    if ( unlikely(guest_cpu_user_regs()->eflags & X86_EFLAGS_VM) )
        return 1;
    if ( hvm_long_mode_enabled(v) && likely(vmcb->cs.attr.fields.l) )
        return 8;
    return (likely(vmcb->cs.attr.fields.db) ? 4 : 2);
}

void
svm_update_host_cr3(struct vcpu *v)
{
    /* SVM doesn't have a HOST_CR3 equivalent to update. */
}

int
svm_update_guest_cr(struct vcpu *v, unsigned int cr)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;
    uint64_t value;
    int ret = 0;

    switch ( cr )
    {
    case 0: {
        unsigned long hw_cr0_mask = 0;

        if ( !(v->arch.hvm_vcpu.guest_cr[0] & X86_CR0_TS) )
        {
            if ( v != current )
                hw_cr0_mask |= X86_CR0_TS;
            else if ( vmcb_get_cr0(vmcb) & X86_CR0_TS )
                svm_fpu_enter(v);
        }

        value = v->arch.hvm_vcpu.guest_cr[0] | hw_cr0_mask;
        if ( !paging_mode_hap(v->domain) )
            value |= X86_CR0_PG | X86_CR0_WP;
        vmcb_set_cr0(vmcb, value);
        break;
    }
    case 2:
        vmcb_set_cr2(vmcb, v->arch.hvm_vcpu.guest_cr[2]);
        break;
    case 3:
        vmcb_set_cr3(vmcb, v->arch.hvm_vcpu.hw_cr[3]);
        if ( !nestedhvm_enabled(v->domain) )
            hvm_asid_flush_vcpu(v);
        else if ( nestedhvm_vmswitch_in_progress(v) )
            ; /* CR3 switches during VMRUN/VMEXIT do not flush the TLB. */
        else
            hvm_asid_flush_vcpu_asid(
                nestedhvm_vcpu_in_guestmode(v)
                ? &vcpu_nestedhvm(v).nv_n2asid : &v->arch.hvm_vcpu.n1asid);
        break;
    case 4:
        value = HVM_CR4_HOST_MASK;
        if ( paging_mode_hap(v->domain) )
            value &= ~X86_CR4_PAE;
        value |= v->arch.hvm_vcpu.guest_cr[4];
        vmcb_set_cr4(vmcb, value);
        break;
    default:
        BUG();
    }

    return ret;
}

void
svm_update_guest_efer(struct vcpu *v)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;
    bool_t lma = !!(v->arch.hvm_vcpu.guest_efer & EFER_LMA);
    uint64_t new_efer;

    new_efer = (v->arch.hvm_vcpu.guest_efer | EFER_SVME) & ~EFER_LME;
    if ( lma )
        new_efer |= EFER_LME;
    vmcb_set_efer(vmcb, new_efer);
}

static void svm_sync_vmcb(struct vcpu *v)
{
}

void
svm_get_segment_register(struct vcpu *v, enum x86_segment seg,
                         struct segment_register *reg)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;

    ASSERT((v == current) || !vcpu_runnable(v));

    switch ( seg )
    {
    case x86_seg_cs:
        memcpy(reg, &vmcb->cs, sizeof(*reg));
        reg->attr.fields.g = reg->limit > 0xFFFFF;
        break;
    case x86_seg_ds:
        memcpy(reg, &vmcb->ds, sizeof(*reg));
        if ( reg->attr.fields.type != 0 )
            reg->attr.fields.type |= 0x1;
        break;
    case x86_seg_es:
        memcpy(reg, &vmcb->es, sizeof(*reg));
        if ( reg->attr.fields.type != 0 )
            reg->attr.fields.type |= 0x1;
        break;
    case x86_seg_fs:
        svm_sync_vmcb(v);
        memcpy(reg, &vmcb->fs, sizeof(*reg));
        if ( reg->attr.fields.type != 0 )
            reg->attr.fields.type |= 0x1;
        break;
    case x86_seg_gs:
        svm_sync_vmcb(v);
        memcpy(reg, &vmcb->gs, sizeof(*reg));
        if ( reg->attr.fields.type != 0 )
            reg->attr.fields.type |= 0x1;
        break;
    case x86_seg_ss:
        memcpy(reg, &vmcb->ss, sizeof(*reg));
        reg->attr.fields.dpl = vmcb->_cpl;
        if ( reg->attr.fields.type == 0 )
            reg->attr.fields.db = 0;
        break;
    case x86_seg_tr:
        svm_sync_vmcb(v);
        memcpy(reg, &vmcb->tr, sizeof(*reg));
        reg->attr.fields.type |= 0x2;
        break;
    case x86_seg_gdtr:
        memcpy(reg, &vmcb->gdtr, sizeof(*reg));
        break;
    case x86_seg_idtr:
        memcpy(reg, &vmcb->idtr, sizeof(*reg));
        break;
    case x86_seg_ldtr:
        svm_sync_vmcb(v);
        memcpy(reg, &vmcb->ldtr, sizeof(*reg));
        break;
    default:
        BUG();
    }
}

void
svm_set_segment_register(struct vcpu *v, enum x86_segment seg,
                         struct segment_register *reg)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;

    ASSERT((v == current) || !vcpu_runnable(v));

    switch ( seg )
    {
    case x86_seg_cs:
    case x86_seg_ds:
    case x86_seg_es:
    case x86_seg_ss: /* cpl */
        vmcb->cleanbits.fields.seg = 0;
        break;
    case x86_seg_gdtr:
    case x86_seg_idtr:
        vmcb->cleanbits.fields.dt = 0;
        break;
    case x86_seg_fs:
    case x86_seg_gs:
    case x86_seg_tr:
    case x86_seg_ldtr:
        break;
    default:
        break;
    }

    switch ( seg )
    {
    case x86_seg_cs:
        memcpy(&vmcb->cs, reg, sizeof(*reg));
        break;
    case x86_seg_ds:
        memcpy(&vmcb->ds, reg, sizeof(*reg));
        break;
    case x86_seg_es:
        memcpy(&vmcb->es, reg, sizeof(*reg));
        break;
    case x86_seg_fs:
        memcpy(&vmcb->fs, reg, sizeof(*reg));
        break;
    case x86_seg_gs:
        memcpy(&vmcb->gs, reg, sizeof(*reg));
        break;
    case x86_seg_ss:
        memcpy(&vmcb->ss, reg, sizeof(*reg));
        vmcb->_cpl = vmcb->ss.attr.fields.dpl;
        break;
    case x86_seg_tr:
        memcpy(&vmcb->tr, reg, sizeof(*reg));
        break;
    case x86_seg_gdtr:
        vmcb->gdtr.base = reg->base;
        vmcb->gdtr.limit = (uint16_t)reg->limit;
        break;
    case x86_seg_idtr:
        vmcb->idtr.base = reg->base;
        vmcb->idtr.limit = (uint16_t)reg->limit;
        break;
    case x86_seg_ldtr:
        memcpy(&vmcb->ldtr, reg, sizeof(*reg));
        break;
    default:
        BUG();
    }

}

static uint64_t svm_get_tsc_offset(uint64_t host_tsc, uint64_t guest_tsc,
    uint64_t ratio)
{
    uint64_t offset;

    if (ratio == DEFAULT_TSC_RATIO)
        return guest_tsc - host_tsc;

    /* calculate hi,lo parts in 64bits to prevent overflow */
    offset = (((host_tsc >> 32U) * (ratio >> 32U)) << 32U) +
          (host_tsc & 0xffffffffULL) * (ratio & 0xffffffffULL);
    return guest_tsc - offset;
}

void
svm_set_tsc_offset(struct vcpu *v, u64 offset)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;
    struct vmcb_struct *n1vmcb, *n2vmcb;
    uint64_t n2_tsc_offset = 0;
    struct domain *d = v->domain;
    uint64_t host_tsc, guest_tsc;

    guest_tsc = hvm_get_guest_tsc(v);

    /* Re-adjust the offset value when TSC_RATIO is available */
    if ( cpu_has_tsc_ratio && d->arch.vtsc ) {
        rdtscll(host_tsc);
        offset = svm_get_tsc_offset(host_tsc, guest_tsc, vcpu_tsc_ratio(v));
    }

    if ( !nestedhvm_enabled(d) ) {
        vmcb_set_tsc_offset(vmcb, offset);
        return;
    }

    n1vmcb = vcpu_nestedhvm(v).nv_n1vmcx;
    n2vmcb = vcpu_nestedhvm(v).nv_n2vmcx;

    if ( nestedhvm_vcpu_in_guestmode(v) ) {
        struct nestedsvm *svm = &vcpu_nestedsvm(v);

        n2_tsc_offset = vmcb_get_tsc_offset(n2vmcb) -
            vmcb_get_tsc_offset(n1vmcb);
        if ( svm->ns_tscratio != DEFAULT_TSC_RATIO ) {
            n2_tsc_offset = svm_get_tsc_offset(guest_tsc,
                guest_tsc + n2_tsc_offset, svm->ns_tscratio);
        }
        vmcb_set_tsc_offset(n1vmcb, offset);
    }

    vmcb_set_tsc_offset(vmcb, offset + n2_tsc_offset);
}

void
svm_set_rdtsc_exiting(struct vcpu *v, bool_t enable)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;
    u32 general1_intercepts = vmcb_get_general1_intercepts(vmcb);

    general1_intercepts &= ~GENERAL1_INTERCEPT_RDTSC;
    if ( enable )
        general1_intercepts |= GENERAL1_INTERCEPT_RDTSC;

    vmcb_set_general1_intercepts(vmcb, general1_intercepts);
}

unsigned int
svm_get_insn_bytes(struct vcpu *v, uint8_t *buf)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;
    unsigned int len = v->arch.hvm_svm.cached_insn_len;

    if ( len != 0 )
    {
        /* Latch and clear the cached instruction. */
        memcpy(buf, vmcb->guest_ins, 15);
        v->arch.hvm_svm.cached_insn_len = 0;
    }

    return len;
}

void
svm_init_hypercall_page(struct domain *d, void *hypercall_page)
{
    char *p;
    int i;

    for ( i = 0; i < (PAGE_SIZE / 32); i++ )
    {
        p = (char *)(hypercall_page + (i * 32));
        *(u8  *)(p + 0) = 0xb8; /* mov imm32, %eax */
        *(u32 *)(p + 1) = i;
        *(u8  *)(p + 5) = 0x0f; /* vmmcall */
        *(u8  *)(p + 6) = 0x01;
        *(u8  *)(p + 7) = 0xd9;
        *(u8  *)(p + 8) = 0xc3; /* ret */
    }

    /* Don't support HYPERVISOR_iret at the moment */
    *(u16 *)(hypercall_page + (__HYPERVISOR_iret * 32)) = 0x0b0f; /* ud2 */
}

static inline void svm_lwp_save(struct vcpu *v)
{
    /* Don't mess up with other guests. Disable LWP for next VCPU. */
    if ( v->arch.hvm_svm.guest_lwp_cfg )
    {
        wrmsrl(MSR_AMD64_LWP_CFG, 0x0);
        wrmsrl(MSR_AMD64_LWP_CBADDR, 0x0);
    }
}

static inline void svm_lwp_load(struct vcpu *v)
{
    /* Only LWP_CFG is reloaded. LWP_CBADDR will be reloaded via xrstor. */
   if ( v->arch.hvm_svm.guest_lwp_cfg ) 
       wrmsrl(MSR_AMD64_LWP_CFG, v->arch.hvm_svm.guest_lwp_cfg);
}

/* Update LWP_CFG MSR (0xc0000105). Return -1 if error; otherwise returns 0. */
static int svm_update_lwp_cfg(struct vcpu *v, uint64_t msr_content)
{
    unsigned int eax, ebx, ecx, edx;
    uint32_t msr_low;
    
    if ( xsave_enabled(v) && cpu_has_lwp )
    {
        hvm_cpuid(0x8000001c, &eax, &ebx, &ecx, &edx);
        msr_low = (uint32_t)msr_content;
        
        /* generate #GP if guest tries to turn on unsupported features. */
        if ( msr_low & ~edx)
            return -1;
        
        wrmsrl(MSR_AMD64_LWP_CFG, msr_content);
        /* CPU might automatically correct reserved bits. So read it back. */
        rdmsrl(MSR_AMD64_LWP_CFG, msr_content);
        v->arch.hvm_svm.guest_lwp_cfg = msr_content;

        /* track nonalzy state if LWP_CFG is non-zero. */
        v->arch.nonlazy_xstate_used = !!(msr_content);
    }

    return 0;
}

static inline void svm_tsc_ratio_save(struct vcpu *v)
{
    /* Other vcpus might not have vtsc enabled. So disable TSC_RATIO here. */
    if ( cpu_has_tsc_ratio && v->domain->arch.vtsc )
        wrmsrl(MSR_AMD64_TSC_RATIO, DEFAULT_TSC_RATIO);
}

static inline void svm_tsc_ratio_load(struct vcpu *v)
{
    if ( cpu_has_tsc_ratio && v->domain->arch.vtsc ) 
        wrmsrl(MSR_AMD64_TSC_RATIO, vcpu_tsc_ratio(v));
}

void svm_ctxt_switch_from(struct vcpu *v)
{

    if (v->context_loaded == 0)
        return;
    v->context_loaded = 0;

    if (!vmexec_fpu_ctxt_switch)
        vcpu_save_fpu(v);

#ifdef __UXEN_vdr__
    svm_save_dr(v);
#endif  /* __UXEN_vdr__ */
#ifdef __UXEN_vpmu__
    vpmu_save(v);
#endif  /* __UXEN_vpmu__ */
    svm_lwp_save(v);
    svm_tsc_ratio_save(v);

    cpumask_clear_cpu(v->processor, v->domain->domain_dirty_cpumask);
    cpumask_clear_cpu(v->processor, v->vcpu_dirty_cpumask);
    /* vmx_unload_vmcs(v); */

    if ( cpu_has_rdtscp && hvm_has_rdtscp(v->domain) )
        wrmsrl(MSR_TSC_AUX, this_cpu(host_msr_tsc_aux));

    if (!vmexec_fpu_ctxt_switch)
        vcpu_restore_fpu_host(v);
}

static void sync_host_state(struct vcpu *v)
{
    uint64_t cr;

    /* vmx_set_host_env(v); */

    cr = read_cr3();
    if (v->arch.cr3 != cr) {
        make_cr3(v, cr);
        hvm_update_host_cr3(v);
    }
}

void svm_ctxt_switch_to(struct vcpu *v)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;
    int cpu = smp_processor_id();

#ifdef DEBUG
    /* ASSERT(!v->context_loaded || v->arch.cr3 == read_cr3()); */
#endif

    if (v->context_loaded != 0)
        return;

    if (!vmexec_fpu_ctxt_switch)
        vcpu_save_fpu_host(v);

    ASSERT(v->is_running);

    vcpu_switch_host_cpu(v);
    svm_do_resume(v);

    sync_host_state(v);

    cpumask_set_cpu(cpu, v->domain->domain_dirty_cpumask);
    cpumask_set_cpu(cpu, v->vcpu_dirty_cpumask);

    pt_maybe_sync_cpu(v->domain);

#ifdef __UXEN_vdr__
    svm_restore_dr(v);
#endif  /* __UXEN_vdr__ */

    if (ax_present)
        ax_svm_vmsave_root(v);
    else
        svm_vmsave(per_cpu(root_vmcb, cpu));
    v->arch.hvm_svm.root_vmcb_pa = __pa(per_cpu(root_vmcb, cpu));
    vmcb->cleanbits.bytes = 0;
#ifdef __UXEN_vpmu__
    vpmu_load(v);
#endif  /* __UXEN_vpmu__ */
    svm_lwp_load(v);
    svm_tsc_ratio_load(v);

    if ( cpu_has_rdtscp && hvm_has_rdtscp(v->domain) ) {
        unsigned long tsc_aux = hvm_msr_tsc_aux(v);
        rdmsrl(MSR_TSC_AUX, this_cpu(host_msr_tsc_aux));
        if (this_cpu(host_msr_tsc_aux) != tsc_aux)
            wrmsrl(MSR_TSC_AUX, hvm_msr_tsc_aux(v));
    }

    v->context_loaded = 1;
}

static void svm_do_resume(struct vcpu *v) 
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;
    bool_t debug_state = v->domain->debugger_attached;
    bool_t vcpu_guestmode = 0;

    if ( nestedhvm_enabled(v->domain) && nestedhvm_vcpu_in_guestmode(v) )
        vcpu_guestmode = 1;

    if ( !vcpu_guestmode &&
        unlikely(v->arch.hvm_vcpu.debug_state_latch != debug_state) )
    {
        uint32_t intercepts = vmcb_get_exception_intercepts(vmcb);

        v->arch.hvm_vcpu.debug_state_latch = debug_state;
        vmcb_set_exception_intercepts(
            vmcb, debug_state ? (intercepts | (1U << TRAP_int3))
                              : (intercepts & ~(1U << TRAP_int3)));
    }

    if ( v->arch.hvm_svm.launch_core != smp_processor_id() )
    {
        v->arch.hvm_svm.launch_core = smp_processor_id();
        hvm_migrate_timers(v);
        hvm_migrate_pirqs(v);
        /* vmx_set_host_env(v); */
        /* Migrating to another ASID domain.  Request a new ASID. */
        hvm_asid_flush_vcpu(v);
    }

    if ( !vcpu_guestmode )
    {
        vintr_t intr;

        /* Reflect the vlapic's TPR in the hardware vtpr */
        intr = vmcb_get_vintr(vmcb);
        intr.fields.tpr =
            (vlapic_get_reg(vcpu_vlapic(v), APIC_TASKPRI) & 0xFF) >> 4;
        vmcb_set_vintr(vmcb, intr);
    }

}

int
svm_domain_initialise(struct domain *d)
{
    return 0;
}

void
svm_domain_destroy(struct domain *d)
{
}

void
svm_domain_relinquish_memory(struct domain *d)
{
}

int
svm_vcpu_initialise(struct vcpu *v)
{
    int rc;

    v->arch.hvm_svm.launch_core = -1;

    if ( (rc = svm_create_vmcb(v)) != 0 )
    {
        dprintk(XENLOG_WARNING,
                "Failed to create VMCB for vcpu vm%u.%u: err=%d.\n",
                v->domain->domain_id, v->vcpu_id, rc);
        return rc;
    }

#ifdef __UXEN_vpmu__
    vpmu_initialise(v);
#endif  /* __UXEN_vpmu__ */
    return 0;
}

void
svm_vcpu_destroy(struct vcpu *v)
{
    svm_destroy_vmcb(v);
#ifdef __UXEN_vpmu__
    vpmu_destroy(v);
#endif  /* __UXEN_vpmu__ */
}

void
svm_inject_exception(unsigned int trapnr, int errcode, unsigned long cr2)
{
    struct vcpu *curr = current;
    struct vmcb_struct *vmcb = curr->arch.hvm_svm.vmcb;
    eventinj_t event = vmcb->eventinj;

    switch ( trapnr )
    {
    case TRAP_debug:
        if ( guest_cpu_user_regs()->eflags & X86_EFLAGS_TF )
        {
            __restore_debug_registers(curr);
            vmcb_set_dr6(vmcb, vmcb_get_dr6(vmcb) | 0x4000);
        }
        if ( !curr->domain->debugger_attached )
            break;
    case TRAP_int3:
        if ( curr->domain->debugger_attached )
        {
            /* Debug/Int3: Trap to debugger. */
            domain_pause_for_debugger();
            return;
        }
    case TRAP_nmi:
        svm_inject_nmi(curr);
        return;
    }

    if ( unlikely(event.fields.v) &&
         (event.fields.type == X86_EVENTTYPE_HW_EXCEPTION) )
    {
        trapnr = hvm_combine_hw_exceptions(event.fields.vector, trapnr);
        if ( trapnr == TRAP_double_fault )
            errcode = 0;
    }

    event.bytes = 0;
    event.fields.v = 1;
    event.fields.type = X86_EVENTTYPE_HW_EXCEPTION;
    event.fields.vector = trapnr;
    event.fields.ev = (errcode != HVM_DELIVER_NO_ERROR_CODE);
    event.fields.errorcode = errcode;

    vmcb->eventinj = event;

    if ( trapnr == TRAP_page_fault )
    {
        curr->arch.hvm_vcpu.guest_cr[2] = cr2;
        vmcb_set_cr2(vmcb, cr2);
        HVMTRACE_LONG_2D(PF_INJECT, errcode, TRC_PAR_LONG(cr2));
    }
    else
    {
        HVMTRACE_2D(INJ_EXC, trapnr, errcode);
    }
}

int
svm_event_pending(struct vcpu *v)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;
    return vmcb->eventinj.fields.v;
}

int
svm_do_pmu_interrupt(struct cpu_user_regs *regs)
{
#ifdef __UXEN_vpmu__
    return vpmu_do_interrupt(regs);
#else  /* __UXEN_vpmu__ */
    return 0;
#endif  /* __UXEN_vpmu__ */
}

void
svm_cpu_dead(unsigned int cpu)
{
    free_xenheap_page(per_cpu(hsa, cpu));
    per_cpu(hsa, cpu) = NULL;
    free_vmcb(per_cpu(root_vmcb, cpu));
    per_cpu(root_vmcb, cpu) = NULL;
}

int
svm_cpu_on(void)
{
    return 0;
}

void
svm_cpu_off(void)
{
}

int
svm_cpu_up_prepare(unsigned int cpu)
{
    if ( ((per_cpu(hsa, cpu) == NULL) &&
          ((per_cpu(hsa, cpu) = alloc_host_save_area()) == NULL)) ||
         ((per_cpu(root_vmcb, cpu) == NULL) &&
          ((per_cpu(root_vmcb, cpu) = alloc_vmcb()) == NULL)) )
    {
        svm_cpu_dead(cpu);
        return -ENOMEM;
    }

    return 0;
}

static void svm_init_erratum_383(struct cpuinfo_x86 *c)
{
    uint64_t msr_content;

    /* check whether CPU is affected */
    if ( !cpu_has_amd_erratum(c, AMD_ERRATUM_383) )
        return;

    /* use safe methods to be compatible with nested virtualization */
    if (rdmsr_safe(MSR_AMD64_DC_CFG, msr_content) == 0 &&
        wrmsr_safe(MSR_AMD64_DC_CFG, msr_content | (1ULL << 47)) == 0)
    {
        amd_erratum383_found = 1;
    } else {
        printk("Failed to enable erratum 383\n");
    }
}

int
svm_cpu_up(enum hvmon hvmon_mode)
{
    uint64_t msr_content;
    int rc, cpu = smp_processor_id();
    struct cpuinfo_x86 *c = &cpu_data[cpu];
 
    /* Check whether SVM feature is disabled in BIOS */
    rdmsrl(MSR_K8_VM_CR, msr_content);
    if ( msr_content & K8_VMCR_SVME_DISABLE )
    {
        printk("CPU%d: AMD SVM Extension is disabled in BIOS.\n", cpu);
        return -EINVAL;
    }

    if (!cpu_has_efer) {
        printk("CPU%d: can't enable AMD SVM without EFER\n", cpu);
        return -EINVAL;
    }

    if ( (rc = svm_cpu_up_prepare(cpu)) != 0 )
        return rc;

    write_efer(read_efer() | EFER_SVME);

    /* Initialize the HSA for this core. */
    wrmsrl(MSR_K8_VM_HSAVE_PA, (uint64_t)virt_to_maddr(per_cpu(hsa, cpu)));

    /* check for erratum 383 */
    svm_init_erratum_383(c);

    /* Initialize core's ASID handling. */
    svm_asid_init(c);

#ifdef __x86_64__
    /*
     * Check whether EFER.LMSLE can be written.
     * Unfortunately there's no feature bit defined for this.
     */
    msr_content = read_efer();
    if ( wrmsr_safe(MSR_EFER, msr_content | EFER_LMSLE) == 0 )
        rdmsrl(MSR_EFER, msr_content);
    if ( msr_content & EFER_LMSLE )
    {
        if ( c == &boot_cpu_data )
            cpu_has_lmsl = 1;
        wrmsrl(MSR_EFER, msr_content ^ EFER_LMSLE);
    }
    else
    {
        if ( cpu_has_lmsl )
            printk(XENLOG_WARNING "Inconsistent LMSLE support across CPUs!\n");
        cpu_has_lmsl = 0;
    }
#endif

    return 0;
}

void
svm_cpu_down(void)
{
    write_efer(read_efer() & ~EFER_SVME);
}

struct hvm_function_table * __init start_svm(void)
{
    bool_t printed = 0;

    if ( !test_bit(X86_FEATURE_SVM, &boot_cpu_data.x86_capability) )
        return NULL;

    if (ax_setup())
        return NULL;

    /* Sanity check hvm_io_bitmap */
    if ((u64)virt_to_maddr(hvm_io_bitmap) + (1 << PAGE_SHIFT) !=
        (u64)virt_to_maddr((char *)hvm_io_bitmap + (1 << PAGE_SHIFT)) ||
        (u64)virt_to_maddr(hvm_io_bitmap) + (2 << PAGE_SHIFT) !=
        (u64)virt_to_maddr((char *)hvm_io_bitmap + (2 << PAGE_SHIFT))) {
        printk("SVM: hvm_io_bitmap not physically contiguous\n");
        return NULL;
    }

    if ( svm_cpu_up(hvmon_default) )
    {
        printk("SVM: failed to initialise.\n");
        return NULL;
    }

    setup_vmcb_dump();

    svm_feature_flags = ((cpuid_eax(0x80000000) >= 0x8000000A) ?
                         cpuid_edx(0x8000000A) : 0);

    printk("SVM: Supported advanced features:\n");

    /* DecodeAssists fast paths assume nextrip is valid for fast rIP update. */
    if ( !cpu_has_svm_nrips )
        clear_bit(SVM_FEATURE_DECODEASSISTS, &svm_feature_flags);

#define P(p,s) if ( p ) { printk(" - %s\n", s); printed = 1; }
    P(cpu_has_svm_npt, "Nested Page Tables (NPT)");
    P(cpu_has_svm_lbrv, "Last Branch Record (LBR) Virtualisation");
    P(cpu_has_svm_nrips, "Next-RIP Saved on #VMEXIT");
    P(cpu_has_svm_cleanbits, "VMCB Clean Bits");
    P(cpu_has_svm_decode, "DecodeAssists");
    P(cpu_has_pause_filter, "Pause-Intercept Filter");
    P(cpu_has_tsc_ratio, "TSC Rate MSR");
#undef P

    if ( !printed )
        printk(" - none\n");

    svm_function_table.hap_supported = cpu_has_svm_npt;
    svm_function_table.hap_capabilities = HVM_HAP_SUPERPAGE_2MB |
        (((CONFIG_PAGING_LEVELS == 4) && (cpuid_edx(0x80000001) & 0x04000000)) ?
            HVM_HAP_SUPERPAGE_1GB : 0);

    _uxen_info.ui_vmi_msrpm_size = MSRPM_SIZE;

    return &svm_function_table;
}

static void svm_do_nested_pgfault(struct vcpu *v,
    struct cpu_user_regs *regs, uint32_t npfec, paddr_t gpa)
{
    int ret;
    unsigned long gfn = gpa >> PAGE_SHIFT;
    mfn_t mfn;
    p2m_type_t p2mt;
    p2m_access_t p2ma;
    struct p2m_domain *p2m = NULL;

    ret = hvm_hap_nested_page_fault(gpa, 0, ~0ul, 
                                    1, /* All NPFs count as reads */
                                    npfec & PFEC_write_access, 
                                    npfec & PFEC_insn_fetch);

    if ( tb_init_done )
    {
        struct {
            uint64_t gpa;
            uint64_t mfn;
            uint32_t qualification;
            uint32_t p2mt;
        } _d;

        p2m = p2m_get_p2m(v);
        _d.gpa = gpa;
        _d.qualification = 0;
        mfn = get_gfn_type_access(p2m, gfn, &_d.p2mt, &p2ma, p2m_query, NULL);
        __put_gfn(p2m, gfn);
        _d.mfn = mfn_x(mfn);
        
        __trace_var(TRC_HVM_NPF, 0, sizeof(_d), &_d);
    }

    switch (ret) {
    case 0:
        break;
    case 1:
        return;
    case -1:
        BUG();
    }

    if ( p2m == NULL )
        p2m = p2m_get_p2m(v);
    /* Everything else is an error. */
    mfn = get_gfn_type_access(p2m, gfn, &p2mt, &p2ma, p2m_guest, NULL);
    __put_gfn(p2m, gfn);
    gdprintk(XENLOG_ERR,
         "SVM violation gpa %#"PRIpaddr", mfn %#lx, type %i\n",
         gpa, mfn_x(mfn), p2mt);
    domain_crash(v->domain);
}

void
svm_fpu_dirty_intercept(void)
{
    struct vcpu *v = current;
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;

    svm_fpu_enter(v);

    if ( !(v->arch.hvm_vcpu.guest_cr[0] & X86_CR0_TS) )
        vmcb_set_cr0(vmcb, vmcb_get_cr0(vmcb) & ~X86_CR0_TS);
}

void
svm_cpuid_intercept(unsigned int *eax, unsigned int *ebx,
                    unsigned int *ecx, unsigned int *edx)
{
    unsigned int input = *eax;
    struct vcpu *v = current;

    hvm_cpuid(input, eax, ebx, ecx, edx);

    switch (input) {
    case 0x80000001:
        /* Fix up VLAPIC details. */
        if ( vlapic_hw_disabled(vcpu_vlapic(v)) )
            __clear_bit(X86_FEATURE_APIC & 31, edx);
        /* No support for OS Visible Workaround OSVW */
        __clear_bit(X86_FEATURE_OSVW & 31, ecx);
        /* No support fot data breakpoint extension DBEXT */
        __clear_bit(X86_FEATURE_DBEXT & 31, ecx);
        break;
    case 0x8000001c: 
    {
        /* LWP capability CPUID */
        uint64_t lwp_cfg = v->arch.hvm_svm.guest_lwp_cfg;

        if ( cpu_has_lwp )
        {
            if ( !(v->arch.xcr0 & XSTATE_LWP) )
           {
                *eax = 0x0;
                break;
            }

            /* turn on available bit and other features specified in lwp_cfg */
            *eax = (*edx & lwp_cfg) | 0x00000001;
        }
        break;
    }
    default:
        break;
    }

    HVMTRACE_5D (CPUID, input, *eax, *ebx, *ecx, *edx);
}

static void svm_vmexit_do_cpuid(struct cpu_user_regs *regs)
{
    unsigned int eax, ebx, ecx, edx, inst_len;

    if ( (inst_len = __get_instruction_length(current, INSTR_CPUID)) == 0 )
        return;

    eax = regs->eax;
    ebx = regs->ebx;
    ecx = regs->ecx;
    edx = regs->edx;

    svm_cpuid_intercept(&eax, &ebx, &ecx, &edx);

    regs->eax = eax;
    regs->ebx = ebx;
    regs->ecx = ecx;
    regs->edx = edx;

    __update_guest_eip(regs, inst_len);
}

static void svm_vmexit_do_cr_access(
    struct vmcb_struct *vmcb, struct cpu_user_regs *regs)
{
    int gp, cr, dir, rc;

    cr = vmcb->exitcode - VMEXIT_CR0_READ;
    dir = (cr > 15);
    cr &= 0xf;
    gp = vmcb->exitinfo1 & 0xf;

    rc = dir ? hvm_mov_to_cr(cr, gp) : hvm_mov_from_cr(cr, gp);

    if ( rc == X86EMUL_OKAY )
        __update_guest_eip(regs, vmcb->nextrip - vmcb->rip);
}

static void svm_dr_access(struct vcpu *v, struct cpu_user_regs *regs)
{
    HVMTRACE_0D(DR_WRITE);
    __restore_debug_registers(v);
}

int
svm_msr_read_intercept(unsigned int msr, uint64_t *msr_content)
{
    struct vcpu *v = current;
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;

    switch ( msr )
    {
    case MSR_IA32_SYSENTER_CS:
        *msr_content = v->arch.hvm_svm.guest_sysenter_cs;
        break;
    case MSR_IA32_SYSENTER_ESP:
        *msr_content = v->arch.hvm_svm.guest_sysenter_esp;
        break;
    case MSR_IA32_SYSENTER_EIP:
        *msr_content = v->arch.hvm_svm.guest_sysenter_eip;
        break;

    case MSR_IA32_MC4_MISC: /* Threshold register */
    case MSR_F10_MC4_MISC1 ... MSR_F10_MC4_MISC3:
        /*
         * MCA/MCE: We report that the threshold register is unavailable
         * for OS use (locked by the BIOS).
         */
        *msr_content = 1ULL << 61; /* MC4_MISC.Locked */
        break;

    case MSR_IA32_EBC_FREQUENCY_ID:
        /*
         * This Intel-only register may be accessed if this HVM guest
         * has been migrated from an Intel host. The value zero is not
         * particularly meaningful, but at least avoids the guest crashing!
         */
        *msr_content = 0;
        break;

    case MSR_IA32_DEBUGCTLMSR:
        *msr_content = vmcb_get_debugctlmsr(vmcb);
        break;

    case MSR_IA32_LASTBRANCHFROMIP:
        *msr_content = vmcb_get_lastbranchfromip(vmcb);
        break;

    case MSR_IA32_LASTBRANCHTOIP:
        *msr_content = vmcb_get_lastbranchtoip(vmcb);
        break;

    case MSR_IA32_LASTINTFROMIP:
        *msr_content = vmcb_get_lastintfromip(vmcb);
        break;

    case MSR_IA32_LASTINTTOIP:
        *msr_content = vmcb_get_lastinttoip(vmcb);
        break;

    case MSR_AMD64_LWP_CFG:
        *msr_content = v->arch.hvm_svm.guest_lwp_cfg;
        break;

    case MSR_K7_PERFCTR0:
    case MSR_K7_PERFCTR1:
    case MSR_K7_PERFCTR2:
    case MSR_K7_PERFCTR3:
    case MSR_K7_EVNTSEL0:
    case MSR_K7_EVNTSEL1:
    case MSR_K7_EVNTSEL2:
    case MSR_K7_EVNTSEL3:
    case MSR_AMD_FAM15H_PERFCTR0:
    case MSR_AMD_FAM15H_PERFCTR1:
    case MSR_AMD_FAM15H_PERFCTR2:
    case MSR_AMD_FAM15H_PERFCTR3:
    case MSR_AMD_FAM15H_PERFCTR4:
    case MSR_AMD_FAM15H_PERFCTR5:
    case MSR_AMD_FAM15H_EVNTSEL0:
    case MSR_AMD_FAM15H_EVNTSEL1:
    case MSR_AMD_FAM15H_EVNTSEL2:
    case MSR_AMD_FAM15H_EVNTSEL3:
    case MSR_AMD_FAM15H_EVNTSEL4:
    case MSR_AMD_FAM15H_EVNTSEL5:
#ifdef __UXEN_vpmu__
        vpmu_do_rdmsr(msr, msr_content);
#else  /* __UXEN_vpmu__ */
        *msr_content = 0;       /* no vPMU */
#endif  /* __UXEN_vpmu__ */
        break;

    case MSR_AMD64_DR0_ADDRESS_MASK:
    case MSR_AMD64_DR1_ADDRESS_MASK ... MSR_AMD64_DR3_ADDRESS_MASK:
        goto gpf;               /* no DBEXT */

    case MSR_AMD_OSVW_ID_LENGTH:
    case MSR_AMD_OSVW_STATUS:
        goto gpf;               /* no OSVW */

    default:

        if ( rdmsr_viridian_regs(msr, msr_content) ||
             rdmsr_hypervisor_regs(msr, msr_content) )
            break;

        if ( rdmsr_safe(msr, *msr_content) == 0 )
            break;

        goto gpf;
    }

    HVM_DBG_LOG(DBG_LEVEL_1, "returns: ecx=%x, msr_value=%"PRIx64,
                msr, *msr_content);
    return X86EMUL_OKAY;

 gpf:
    hvm_inject_exception(TRAP_gp_fault, 0, 0);
    return X86EMUL_EXCEPTION;
}

int
svm_msr_write_intercept(unsigned int msr, uint64_t msr_content)
{
    struct vcpu *v = current;
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;
    int ret = X86EMUL_OKAY;
    int r;

    switch ( msr )
    {
    case MSR_IA32_SYSENTER_CS:
    case MSR_IA32_SYSENTER_ESP:
    case MSR_IA32_SYSENTER_EIP:
        break;
    default:
        break;
    }

    switch ( msr )
    {
    case MSR_IA32_SYSENTER_CS:
        vmcb->sysenter_cs = v->arch.hvm_svm.guest_sysenter_cs = msr_content;
        break;
    case MSR_IA32_SYSENTER_ESP:
        vmcb->sysenter_esp = v->arch.hvm_svm.guest_sysenter_esp = msr_content;
        break;
    case MSR_IA32_SYSENTER_EIP:
        vmcb->sysenter_eip = v->arch.hvm_svm.guest_sysenter_eip = msr_content;
        break;

    case MSR_IA32_DEBUGCTLMSR:
        vmcb_set_debugctlmsr(vmcb, msr_content);
        if ( !msr_content || !cpu_has_svm_lbrv )
            break;
        vmcb->lbr_control.fields.enable = 1;
        svm_disable_intercept_for_msr(v, MSR_IA32_DEBUGCTLMSR);
        svm_disable_intercept_for_msr(v, MSR_IA32_LASTBRANCHFROMIP);
        svm_disable_intercept_for_msr(v, MSR_IA32_LASTBRANCHTOIP);
        svm_disable_intercept_for_msr(v, MSR_IA32_LASTINTFROMIP);
        svm_disable_intercept_for_msr(v, MSR_IA32_LASTINTTOIP);
        break;

    case MSR_IA32_LASTBRANCHFROMIP:
        vmcb_set_lastbranchfromip(vmcb, msr_content);
        break;

    case MSR_IA32_LASTBRANCHTOIP:
        vmcb_set_lastbranchtoip(vmcb, msr_content);
        break;

    case MSR_IA32_LASTINTFROMIP:
        vmcb_set_lastintfromip(vmcb, msr_content);
        break;

    case MSR_IA32_LASTINTTOIP:
        vmcb_set_lastinttoip(vmcb, msr_content);
        break;

    case MSR_AMD64_LWP_CFG:
        if ( svm_update_lwp_cfg(v, msr_content) < 0 )
            goto gpf;
        break;

    case MSR_K7_PERFCTR0:
    case MSR_K7_PERFCTR1:
    case MSR_K7_PERFCTR2:
    case MSR_K7_PERFCTR3:
    case MSR_K7_EVNTSEL0:
    case MSR_K7_EVNTSEL1:
    case MSR_K7_EVNTSEL2:
    case MSR_K7_EVNTSEL3:
    case MSR_AMD_FAM15H_PERFCTR0:
    case MSR_AMD_FAM15H_PERFCTR1:
    case MSR_AMD_FAM15H_PERFCTR2:
    case MSR_AMD_FAM15H_PERFCTR3:
    case MSR_AMD_FAM15H_PERFCTR4:
    case MSR_AMD_FAM15H_PERFCTR5:
    case MSR_AMD_FAM15H_EVNTSEL0:
    case MSR_AMD_FAM15H_EVNTSEL1:
    case MSR_AMD_FAM15H_EVNTSEL2:
    case MSR_AMD_FAM15H_EVNTSEL3:
    case MSR_AMD_FAM15H_EVNTSEL4:
    case MSR_AMD_FAM15H_EVNTSEL5:
#ifdef __UXEN_vpmu__
        vpmu_do_wrmsr(msr, msr_content);
#endif  /* __UXEN_vpmu__ */
        break;

     case MSR_AMD64_DR0_ADDRESS_MASK:
     case MSR_AMD64_DR1_ADDRESS_MASK ... MSR_AMD64_DR3_ADDRESS_MASK:
         goto gpf;              /* no DBEXT */

     case MSR_AMD_OSVW_ID_LENGTH:
     case MSR_AMD_OSVW_STATUS:
         goto gpf;              /* no OSVW */

    case MSR_IA32_MC4_MISC: /* Threshold register */
    case MSR_F10_MC4_MISC1 ... MSR_F10_MC4_MISC3:
        /*
         * MCA/MCE: Threshold register is reported to be locked, so we ignore
         * all write accesses. This behaviour matches real HW, so guests should
         * have no problem with this.
         */
        break;

    default:

        r = wrmsr_viridian_regs(msr, msr_content);
        if (!r)
            r = wrmsr_hypervisor_regs(msr, msr_content);
        if (r == -1)
            ret = X86EMUL_RETRY;
        break;
    }

    return ret;

 gpf:
    hvm_inject_exception(TRAP_gp_fault, 0, 0);
    return X86EMUL_EXCEPTION;
}

static void svm_do_msr_access(struct cpu_user_regs *regs)
{
    int rc, inst_len;
    struct vcpu *v = current;
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;
    uint64_t msr_content;

    if ( vmcb->exitinfo1 == 0 )
    {
        if ( (inst_len = __get_instruction_length(v, INSTR_RDMSR)) == 0 )
            return;
        rc = hvm_msr_read_intercept(regs->ecx, &msr_content);
        regs->eax = (uint32_t)msr_content;
        regs->edx = (uint32_t)(msr_content >> 32);
    }
    else
    {
        if ( (inst_len = __get_instruction_length(v, INSTR_WRMSR)) == 0 )
            return;
        msr_content = ((uint64_t)regs->edx << 32) | (uint32_t)regs->eax;
        rc = hvm_msr_write_intercept(regs->ecx, msr_content);
    }

    if ( rc == X86EMUL_OKAY )
        __update_guest_eip(regs, inst_len);
}

static void svm_vmexit_do_hlt(struct vmcb_struct *vmcb,
                              struct cpu_user_regs *regs)
{
    unsigned int inst_len;

    if ( (inst_len = __get_instruction_length(current, INSTR_HLT)) == 0 )
        return;
    __update_guest_eip(regs, inst_len);

    hvm_hlt(regs->eflags);
}

static void svm_vmexit_do_rdtsc(struct cpu_user_regs *regs)
{
    unsigned int inst_len;

    if ( (inst_len = __get_instruction_length(current, INSTR_RDTSC)) == 0 )
        return;
    __update_guest_eip(regs, inst_len);

    hvm_rdtsc_intercept(regs);
}

static void svm_vmexit_do_pause(struct cpu_user_regs *regs)
{
    unsigned int inst_len;

    if ( (inst_len = __get_instruction_length(current, INSTR_PAUSE)) == 0 )
        return;
    __update_guest_eip(regs, inst_len);

    /*
     * The guest is running a contended spinlock and we've detected it.
     * Do something useful, like reschedule the guest
     */
    perfc_incr(pauseloop_exits);
    do_sched_op(SCHEDOP_yield, XEN_GUEST_HANDLE_NULL(void));
}

static void
svm_vmexit_do_vmrun(struct cpu_user_regs *regs,
                    struct vcpu *v, uint64_t vmcbaddr)
{
    if (!nestedhvm_enabled(v->domain)) {
        gdprintk(XENLOG_ERR, "VMRUN: nestedhvm disabled, injecting #UD\n");
        hvm_inject_exception(TRAP_invalid_op, HVM_DELIVER_NO_ERROR_CODE, 0);
        return;
    }

    BUG();
}

static void
svm_vmexit_do_vmload(struct vmcb_struct *vmcb,
                     struct cpu_user_regs *regs,
                     struct vcpu *v, uint64_t vmcbaddr)
{
    int ret;
    unsigned int inst_len;

    if ( (inst_len = __get_instruction_length(v, INSTR_VMLOAD)) == 0 )
        return;

    if (!nestedhvm_enabled(v->domain)) {
        gdprintk(XENLOG_ERR, "VMLOAD: nestedhvm disabled, injecting #UD\n");
        ret = TRAP_invalid_op;
        goto inject;
    }

    BUG();

 inject:
    hvm_inject_exception(ret, HVM_DELIVER_NO_ERROR_CODE, 0);
    return;
}

static void
svm_vmexit_do_vmsave(struct vmcb_struct *vmcb,
                     struct cpu_user_regs *regs,
                     struct vcpu *v, uint64_t vmcbaddr)
{
    int ret;
    unsigned int inst_len;

    if ( (inst_len = __get_instruction_length(v, INSTR_VMSAVE)) == 0 )
        return;

    if (!nestedhvm_enabled(v->domain)) {
        gdprintk(XENLOG_ERR, "VMSAVE: nestedhvm disabled, injecting #UD\n");
        ret = TRAP_invalid_op;
        goto inject;
    }

    BUG();

 inject:
    hvm_inject_exception(ret, HVM_DELIVER_NO_ERROR_CODE, 0);
    return;
}

static void svm_vmexit_ud_intercept(struct cpu_user_regs *regs)
{
    struct hvm_emulate_ctxt ctxt;
    int rc;

    hvm_emulate_prepare(&ctxt, regs);

    rc = hvm_emulate_one(&ctxt);

    switch ( rc )
    {
    case X86EMUL_UNHANDLEABLE:
        hvm_inject_exception(TRAP_invalid_op, HVM_DELIVER_NO_ERROR_CODE, 0);
        break;
    case X86EMUL_EXCEPTION:
        if ( ctxt.exn_pending )
            hvm_inject_exception(ctxt.exn_vector, ctxt.exn_error_code, 0);
        /* fall through */
    default:
        hvm_emulate_writeback(&ctxt);
        break;
    }
}

static int svm_is_erratum_383(struct cpu_user_regs *regs)
{
    uint64_t msr_content;
    struct vcpu *v = current;

    if ( !amd_erratum383_found )
        return 0;

    rdmsrl(MSR_IA32_MC0_STATUS, msr_content);
    /* Bit 62 may or may not be set for this mce */
    msr_content &= ~(1ULL << 62);

    if ( msr_content != 0xb600000000010015ULL )
        return 0;
    
    rdmsrl(MSR_IA32_MCG_STATUS, msr_content);
    wrmsrl(MSR_IA32_MCG_STATUS, msr_content & ~(1ULL << 2));

    /* flush TLB */
    flush_tlb_mask(v->domain->domain_dirty_cpumask);

    return 1;
}

static void svm_vmexit_mce_intercept(
    struct vcpu *v, struct cpu_user_regs *regs)
{
    if ( svm_is_erratum_383(regs) )
    {
        gdprintk(XENLOG_ERR, "SVM hits AMD erratum 383\n");
        domain_crash(v->domain);
    }
}

void
svm_wbinvd_intercept(void)
{
}

static void svm_vmexit_do_invalidate_cache(struct cpu_user_regs *regs)
{
    enum instruction_index list[] = { INSTR_INVD, INSTR_WBINVD };
    int inst_len;

    inst_len = __get_instruction_length_from_list(
        current, list, ARRAY_SIZE(list));
    if ( inst_len == 0 )
        return;

    svm_wbinvd_intercept();

    __update_guest_eip(regs, inst_len);
}

static void svm_invlpga_intercept(
    struct vcpu *v, unsigned long vaddr, uint32_t asid)
{
    svm_invlpga(vaddr,
                (asid == 0)
                ? v->arch.hvm_vcpu.n1asid.asid
                : vcpu_nestedhvm(v).nv_n2asid.asid);
}

void
svm_invlpg_intercept(unsigned long vaddr)
{
    struct vcpu *curr = current;
    HVMTRACE_LONG_2D(INVLPG, 0, TRC_PAR_LONG(vaddr));
    paging_invlpg(curr, vaddr);
    svm_asid_g_invlpg(curr, vaddr);
}

/* Caller must hold pt_sync_lock */
void
svm_pt_maybe_sync_cpu_no_lock(struct domain *d, unsigned int cpu)
{

    if (!cpumask_test_cpu(cpu, d->arch.hvm_domain.pt_synced)) {
        struct p2m_domain *p2m = p2m_get_hostp2m(d);

        cpumask_set_cpu(cpu, d->arch.hvm_domain.pt_synced);

        flush_tlb_local();
        p2m->virgin = 1;
    }
}

bool_t
svm_ple_enabled(struct vcpu *v)
{
    return !!(v->arch.hvm_svm.vmcb->_general1_intercepts &
              GENERAL1_INTERCEPT_PAUSE);
}

void
svm_dump_vcpu(struct vcpu *v, const char *from)
{
    svm_vmcb_dump(from, v->arch.hvm_svm.vmcb);
}

#ifdef __x86_64__
#define GUEST_OS_PER_CPU_SEGMENT_BASE(vmcb) vmcb->gs.base
#else
#define GUEST_OS_PER_CPU_SEGMENT_BASE(vmcb) vmcb->fs.base
#endif

uintptr_t
svm_exit_info(struct vcpu *v, unsigned int field)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;
    uintptr_t ret = 0;

    switch (field) {
    case EXIT_INFO_per_cpu_segment_base:
        ret = GUEST_OS_PER_CPU_SEGMENT_BASE(vmcb);
        break;
    }

    return ret;
}

static struct hvm_function_table __read_mostly svm_function_table = {
    .name = "SVM",
};

void
svm_do_execute(struct vcpu *v)
{
    uint64_t exit_reason;
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;
    eventinj_t eventinj;
    int inst_len, rc;
    vintr_t intr;
    bool_t vcpu_guestmode = 0;
    struct cpu_user_regs *regs = guest_cpu_user_regs();

    ASSERT(v);

    if (!ax_present && svm_asm_do_vmentry(v))
        return;
    if (ax_present && ax_svm_vmrun(v, vmcb, regs))
        return;

    if ( paging_mode_hap(v->domain) ) {
        struct p2m_domain *p2m = p2m_get_hostp2m(v->domain);

        v->arch.hvm_vcpu.guest_cr[3] = v->arch.hvm_vcpu.hw_cr[3] =
            vmcb_get_cr3(vmcb);
        p2m->virgin = 0;
    }

    if ( nestedhvm_enabled(v->domain) && nestedhvm_vcpu_in_guestmode(v) )
        vcpu_guestmode = 1;

    /*
     * Before doing anything else, we need to sync up the VLAPIC's TPR with
     * SVM's vTPR. It's OK if the guest doesn't touch CR8 (e.g. 32-bit Windows)
     * because we update the vTPR on MMIO writes to the TPR.
     * NB. We need to preserve the low bits of the TPR to make checked builds
     * of Windows work, even though they don't actually do anything.
     */
    if ( !vcpu_guestmode ) {
        intr = vmcb_get_vintr(vmcb);
        vlapic_set_reg(vcpu_vlapic(v), APIC_TASKPRI,
                   ((intr.fields.tpr & 0x0F) << 4) |
                   (vlapic_get_reg(vcpu_vlapic(v), APIC_TASKPRI) & 0x0F));
    }

    exit_reason = vmcb->exitcode;

    if ( hvm_long_mode_enabled(v) )
        HVMTRACE_ND(VMEXIT64, vcpu_guestmode ? TRC_HVM_NESTEDFLAG : 0,
                    1/*cycles*/, 3, exit_reason,
                    (uint32_t)regs->eip, (uint32_t)((uint64_t)regs->eip >> 32),
                    0, 0, 0);
    else
        HVMTRACE_ND(VMEXIT, vcpu_guestmode ? TRC_HVM_NESTEDFLAG : 0,
                    1/*cycles*/, 2, exit_reason,
                    (uint32_t)regs->eip,
                    0, 0, 0, 0);

    if ( unlikely(exit_reason == VMEXIT_INVALID) )
    {
        svm_vmcb_dump(__func__, vmcb);
        goto exit_and_crash;
    }

    perfc_incra(svmexits, exit_reason);

    if (exit_reason < ARRAY_SIZE(v->vmexit_reason_count)) {
        v->vmexit_reason_count[(uint16_t)exit_reason]++;
        if ((v->vmexit_reason_count[(uint16_t)exit_reason] % 500000) == 0)
            printk("vm%u.%u: 500k reason %d\n", v->domain->domain_id,
                   v->vcpu_id, (uint16_t)exit_reason);
    }

    vmcb->cleanbits.bytes = cpu_has_svm_cleanbits ? ~0u : 0u;

    /* Event delivery caused this intercept? Queue for redelivery. */
    eventinj = vmcb->exitintinfo;
    if ( unlikely(eventinj.fields.v) &&
         hvm_event_needs_reinjection(eventinj.fields.type,
                                     eventinj.fields.vector) )
        vmcb->eventinj = eventinj;

    switch ( exit_reason )
    {
    case VMEXIT_INTR:
        /* Asynchronous event, handled when we STGI'd after the VMEXIT. */
        HVMTRACE_0D(INTR);
        v->force_preempt = 1;
        break;

    case VMEXIT_NMI:
        /* Asynchronous event, handled when we STGI'd after the VMEXIT. */
        HVMTRACE_0D(NMI);
        v->force_preempt = 1;
        break;

    case VMEXIT_SMI:
        /* Asynchronous event, handled when we STGI'd after the VMEXIT. */
        HVMTRACE_0D(SMI);
        v->force_preempt = 1;
        break;

    case VMEXIT_EXCEPTION_DB:
        if ( !v->domain->debugger_attached )
            hvm_inject_exception(TRAP_debug, HVM_DELIVER_NO_ERROR_CODE, 0);
        else
            domain_pause_for_debugger();
        break;

    case VMEXIT_EXCEPTION_BP:
        if ( !v->domain->debugger_attached )
            goto exit_and_crash;
        /* AMD Vol2, 15.11: INT3, INTO, BOUND intercepts do not update RIP. */
        if ( (inst_len = __get_instruction_length(v, INSTR_INT3)) == 0 )
            break;
        __update_guest_eip(regs, inst_len);
        current->arch.gdbsx_vcpu_event = TRAP_int3;
        domain_pause_for_debugger();
        break;

    case VMEXIT_EXCEPTION_NM:
        svm_fpu_dirty_intercept();
        break;  

    case VMEXIT_EXCEPTION_PF: {
        unsigned long va;
        va = vmcb->exitinfo2;
        regs->error_code = vmcb->exitinfo1;
        HVM_DBG_LOG(DBG_LEVEL_VMMU,
                    "eax=%lx, ebx=%lx, ecx=%lx, edx=%lx, esi=%lx, edi=%lx",
                    (unsigned long)regs->eax, (unsigned long)regs->ebx,
                    (unsigned long)regs->ecx, (unsigned long)regs->edx,
                    (unsigned long)regs->esi, (unsigned long)regs->edi);

        if ( cpu_has_svm_decode )
            v->arch.hvm_svm.cached_insn_len = vmcb->guest_ins_len & 0xf;
        rc = paging_fault(va, regs);
        v->arch.hvm_svm.cached_insn_len = 0;

        if ( rc )
        {
            if ( trace_will_trace_event(TRC_SHADOW) )
                break;
            if ( hvm_long_mode_enabled(v) )
                HVMTRACE_LONG_2D(PF_XEN, regs->error_code, TRC_PAR_LONG(va));
            else
                HVMTRACE_2D(PF_XEN, regs->error_code, va);
            break;
        }

        hvm_inject_exception(TRAP_page_fault, regs->error_code, va);
        break;
    }

    case VMEXIT_EXCEPTION_AC:
        hvm_inject_exception(TRAP_alignment_check, vmcb->exitinfo1, 0);
        break;

    case VMEXIT_EXCEPTION_UD:
        svm_vmexit_ud_intercept(regs);
        break;

    /* Asynchronous event, handled when we STGI'd after the VMEXIT. */
    case VMEXIT_EXCEPTION_MC:
        HVMTRACE_0D(MCE);
        svm_vmexit_mce_intercept(v, regs);
        v->force_preempt = 1;
        break;

    case VMEXIT_VINTR: {
        u32 general1_intercepts = vmcb_get_general1_intercepts(vmcb);
        intr = vmcb_get_vintr(vmcb);

        intr.fields.irq = 0;
        general1_intercepts &= ~GENERAL1_INTERCEPT_VINTR;

        vmcb_set_vintr(vmcb, intr);
        vmcb_set_general1_intercepts(vmcb, general1_intercepts);
        break;
    }

    case VMEXIT_INVD:
    case VMEXIT_WBINVD:
        svm_vmexit_do_invalidate_cache(regs);
        break;

    case VMEXIT_TASK_SWITCH: {
        enum hvm_task_switch_reason reason;
        int32_t errcode = -1;
        if ( (vmcb->exitinfo2 >> 36) & 1 )
            reason = TSW_iret;
        else if ( (vmcb->exitinfo2 >> 38) & 1 )
            reason = TSW_jmp;
        else
            reason = TSW_call_or_int;
        if ( (vmcb->exitinfo2 >> 44) & 1 )
            errcode = (uint32_t)vmcb->exitinfo2;

        /*
         * Some processors set the EXITINTINFO field when the task switch
         * is caused by a task gate in the IDT. In this case we will be
         * emulating the event injection, so we do not want the processor
         * to re-inject the original event!
         */
        vmcb->eventinj.bytes = 0;

        hvm_task_switch((uint16_t)vmcb->exitinfo1, reason, errcode);
        break;
    }

    case VMEXIT_CPUID:
        svm_vmexit_do_cpuid(regs);
        break;

    case VMEXIT_HLT:
        svm_vmexit_do_hlt(vmcb, regs);
        break;

    case VMEXIT_IOIO:
        if ( (vmcb->exitinfo1 & (1u<<2)) == 0 )
        {
            uint16_t port = (vmcb->exitinfo1 >> 16) & 0xFFFF;
            int bytes = ((vmcb->exitinfo1 >> 4) & 0x07);
            int dir = (vmcb->exitinfo1 & 1) ? IOREQ_READ : IOREQ_WRITE;
            if ( handle_pio(port, bytes, dir) )
                __update_guest_eip(regs, vmcb->exitinfo2 - vmcb->rip);
        }
        else if ( !handle_mmio() )
            hvm_inject_exception(TRAP_gp_fault, 0, 0);
        break;

    case VMEXIT_CR0_READ ... VMEXIT_CR15_READ:
    case VMEXIT_CR0_WRITE ... VMEXIT_CR15_WRITE:
        if ( cpu_has_svm_decode && (vmcb->exitinfo1 & (1ULL << 63)) )
            svm_vmexit_do_cr_access(vmcb, regs);
        else if ( !handle_mmio() ) 
            hvm_inject_exception(TRAP_gp_fault, 0, 0);
        break;

    case VMEXIT_INVLPG:
        if ( cpu_has_svm_decode )
        {
            svm_invlpg_intercept(vmcb->exitinfo1);
            __update_guest_eip(regs, vmcb->nextrip - vmcb->rip);
        }
        else if ( !handle_mmio() )
            hvm_inject_exception(TRAP_gp_fault, 0, 0);
        break;

    case VMEXIT_INVLPGA:
        if ( (inst_len = __get_instruction_length(v, INSTR_INVLPGA)) == 0 )
            break;
        svm_invlpga_intercept(v, regs->eax, regs->ecx);
        __update_guest_eip(regs, inst_len);
        break;

    case VMEXIT_VMMCALL:
        if ( (inst_len = __get_instruction_length(v, INSTR_VMCALL)) == 0 )
            break;
        BUG_ON(vcpu_guestmode);
        HVMTRACE_1D(VMMCALL, regs->eax);
        rc = hvm_do_hypercall(regs);
        if ( rc != HVM_HCALL_preempted )
        {
            __update_guest_eip(regs, inst_len);
        }
        break;

    case VMEXIT_DR0_READ ... VMEXIT_DR7_READ:
    case VMEXIT_DR0_WRITE ... VMEXIT_DR7_WRITE:
        svm_dr_access(v, regs);
        break;

    case VMEXIT_MSR:
        svm_do_msr_access(regs);
        break;

    case VMEXIT_SHUTDOWN:
        hvm_triple_fault();
        break;

    case VMEXIT_RDTSCP:
        regs->ecx = hvm_msr_tsc_aux(v);
        /* fall through */
    case VMEXIT_RDTSC:
        svm_vmexit_do_rdtsc(regs);
        break;

    case VMEXIT_MONITOR:
    case VMEXIT_MWAIT:
        hvm_inject_exception(TRAP_invalid_op, HVM_DELIVER_NO_ERROR_CODE, 0);
        break;

    case VMEXIT_VMRUN:
        svm_vmexit_do_vmrun(regs, v, regs->eax);
        break;
    case VMEXIT_VMLOAD:
        svm_vmexit_do_vmload(vmcb, regs, v, regs->eax);
        break;
    case VMEXIT_VMSAVE:
        svm_vmexit_do_vmsave(vmcb, regs, v, regs->eax);
        break;
    case VMEXIT_STGI:
        svm_vmexit_do_stgi(regs, v);
        break;
    case VMEXIT_CLGI:
        svm_vmexit_do_clgi(regs, v);
        break;
    case VMEXIT_SKINIT:
        hvm_inject_exception(TRAP_invalid_op, HVM_DELIVER_NO_ERROR_CODE, 0);
        break;

    case VMEXIT_XSETBV:
        if ( (inst_len = __get_instruction_length(current, INSTR_XSETBV))==0 )
            break;
        if ( hvm_handle_xsetbv((((u64)regs->edx) << 32) | regs->eax) == 0 )
            __update_guest_eip(regs, inst_len);
        break;

    case VMEXIT_NPF:
        perfc_incra(svmexits, VMEXIT_NPF_PERFC);
        if ( cpu_has_svm_decode )
            v->arch.hvm_svm.cached_insn_len = vmcb->guest_ins_len & 0xf;
        svm_do_nested_pgfault(v, regs, vmcb->exitinfo1, vmcb->exitinfo2);
        v->arch.hvm_svm.cached_insn_len = 0;
        break;

    case VMEXIT_IRET: {
        u32 general1_intercepts = vmcb_get_general1_intercepts(vmcb);

        /*
         * IRET clears the NMI mask. However because we clear the mask
         * /before/ executing IRET, we set the interrupt shadow to prevent
         * a pending NMI from being injected immediately. This will work
         * perfectly unless the IRET instruction faults: in that case we
         * may inject an NMI before the NMI handler's IRET instruction is
         * retired.
         */
        general1_intercepts &= ~GENERAL1_INTERCEPT_IRET;
        vmcb->interrupt_shadow = 1;

        vmcb_set_general1_intercepts(vmcb, general1_intercepts);
        break;
    }

    case VMEXIT_PAUSE:
        svm_vmexit_do_pause(regs);
        break;

    default:
    exit_and_crash:
        gdprintk(XENLOG_ERR, "unexpected VMEXIT: exit reason = 0x%"PRIx64", "
                 "exitinfo1 = %"PRIx64", exitinfo2 = %"PRIx64"\n",
                 exit_reason, 
                 (u64)vmcb->exitinfo1, (u64)vmcb->exitinfo2);
        domain_crash(v->domain);
        break;
    }

    if ( vcpu_guestmode )
        /* Don't clobber TPR of the nested guest. */
        return;

    /* The exit may have updated the TPR: reflect this in the hardware vtpr */
    intr = vmcb_get_vintr(vmcb);
    intr.fields.tpr =
        (vlapic_get_reg(vcpu_vlapic(v), APIC_TASKPRI) & 0xFF) >> 4;
    vmcb_set_vintr(vmcb, intr);
}

asmlinkage_abi void svm_trace_vmentry(void)
{
    struct vcpu *curr = current;
    HVMTRACE_ND(VMENTRY,
                nestedhvm_vcpu_in_guestmode(curr) ? TRC_HVM_NESTEDFLAG : 0,
                1/*cycles*/, 0, 0, 0, 0, 0, 0, 0);
}
  
asmlinkage_abi void svm_restore_regs(void)
{

    if (!vmexec_fpu_ctxt_switch) {
        vcpu_restore_fpu_lazy(current);
        assert_xcr0_state(XCR0_STATE_VM);
    }

    pt_maybe_sync_cpu_enter(current->domain);
}

asmlinkage_abi void svm_save_regs(void)
{

    pt_maybe_sync_cpu_leave(current->domain);
}

void
svm_do_suspend(struct vcpu *v)
{
}

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
