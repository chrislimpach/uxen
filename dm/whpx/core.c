/*
 * QEMU Windows Hypervisor Platform accelerator (WHPX)
 *
 * Copyright Microsoft Corp. 2017
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 *
 */
/*
 * uXen changes:
 *
 * Copyright 2018, Bromium, Inc.
 * Author: Tomasz Wroblewski <tomasz.wroblewski@bromium.com>
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

#include <dm/qemu_glue.h>
#include <dm/os.h>
#include <dm/cpu.h>
#include <dm/whpx/apic.h>
#include "whpx.h"
#include "core.h"
#include "winhvglue.h"
#include "winhvplatform.h"
#include "winhvemulation.h"
#include "emulate.h"
#include "viridian.h"
#include "util.h"
#include "ioapic.h"
#include <whpx-shared.h>

#define CPUID_DEBUG_OUT_8  0x54545400
#define CPUID_DEBUG_OUT_32 0x54545404

/* matches uxen definitions */
#define WHPXMEM_share_zero_pages 50

struct whpx_memory_share_zero_pages {
    uint64_t gpfn_list_gpfn;
    uint32_t nr_gpfns;
};

//#define EMU_MICROSOFT

// save/restore broken with Xsave on because the API does not export XCR0 register
// and it's impossible to properly save/restore it. Disable until added to API
//#define XSAVE_DISABLED_UNTIL_FIXED

/* vcpu dirty states */

/* emulation has dirtied a subset registers in CPUState */
#define VCPU_DIRTY_EMU           (1UL << 0)
/* registers in CPUState have been dirtied, hyper-v registers need sync */
#define VCPU_DIRTY_CPUSTATE      (1UL << 1)
/* hyper-v registers have been dirtied, CPUstate registers need sync */
#define VCPU_DIRTY_HV            (1UL << 2)

struct whpx_state {
    uint64_t mem_quota;
    WHV_PARTITION_HANDLE partition;
    uint64_t dm_features;
    uint64_t seed_lo, seed_hi;
};

/* registers synchronized with qemu's CPUState */
static const WHV_REGISTER_NAME whpx_register_names[] = {

    /* X64 General purpose registers */
    WHvX64RegisterRax,
    WHvX64RegisterRcx,
    WHvX64RegisterRdx,
    WHvX64RegisterRbx,
    WHvX64RegisterRsp,
    WHvX64RegisterRbp,
    WHvX64RegisterRsi,
    WHvX64RegisterRdi,
    WHvX64RegisterR8,
    WHvX64RegisterR9,
    WHvX64RegisterR10,
    WHvX64RegisterR11,
    WHvX64RegisterR12,
    WHvX64RegisterR13,
    WHvX64RegisterR14,
    WHvX64RegisterR15,
    WHvX64RegisterRip,
    WHvX64RegisterRflags,

    /* X64 Segment registers */
    WHvX64RegisterEs,
    WHvX64RegisterCs,
    WHvX64RegisterSs,
    WHvX64RegisterDs,
    WHvX64RegisterFs,
    WHvX64RegisterGs,
    WHvX64RegisterLdtr,
    WHvX64RegisterTr,

    /* X64 Table registers */
    WHvX64RegisterIdtr,
    WHvX64RegisterGdtr,

    /* X64 Control Registers */
    WHvX64RegisterCr0,
    WHvX64RegisterCr2,
    WHvX64RegisterCr3,
    WHvX64RegisterCr4,
    // WHvX64RegisterCr8,

    /* X64 Debug Registers */
    /*
     * WHvX64RegisterDr0,
     * WHvX64RegisterDr1,
     * WHvX64RegisterDr2,
     * WHvX64RegisterDr3,
     * WHvX64RegisterDr6,
     * WHvX64RegisterDr7,
     */

    /* X64 Floating Point and Vector Registers */
    WHvX64RegisterXmm0,
    WHvX64RegisterXmm1,
    WHvX64RegisterXmm2,
    WHvX64RegisterXmm3,
    WHvX64RegisterXmm4,
    WHvX64RegisterXmm5,
    WHvX64RegisterXmm6,
    WHvX64RegisterXmm7,
    WHvX64RegisterXmm8,
    WHvX64RegisterXmm9,
    WHvX64RegisterXmm10,
    WHvX64RegisterXmm11,
    WHvX64RegisterXmm12,
    WHvX64RegisterXmm13,
    WHvX64RegisterXmm14,
    WHvX64RegisterXmm15,
    WHvX64RegisterFpMmx0,
    WHvX64RegisterFpMmx1,
    WHvX64RegisterFpMmx2,
    WHvX64RegisterFpMmx3,
    WHvX64RegisterFpMmx4,
    WHvX64RegisterFpMmx5,
    WHvX64RegisterFpMmx6,
    WHvX64RegisterFpMmx7,
    WHvX64RegisterFpControlStatus,
    WHvX64RegisterXmmControlStatus,

    /* X64 MSRs */
//    WHvX64RegisterTsc,
    WHvX64RegisterEfer,
#ifdef TARGET_X86_64
    WHvX64RegisterKernelGsBase,
#endif
//    WHvX64RegisterApicBase,
    /* WHvX64RegisterPat, */
    WHvX64RegisterSysenterCs,
    WHvX64RegisterSysenterEip,
    WHvX64RegisterSysenterEsp,
    WHvX64RegisterStar,
#ifdef TARGET_X86_64
    WHvX64RegisterLstar,
    WHvX64RegisterCstar,
    WHvX64RegisterSfmask,
#endif

    /* Interrupt / Event Registers */
    /*
     * WHvRegisterPendingInterruption,
     * WHvRegisterInterruptState,
     * WHvRegisterPendingEvent0,
     * WHvRegisterPendingEvent1
     * WHvX64RegisterDeliverabilityNotifications,
     */
};

struct whpx_register_set {
    WHV_REGISTER_VALUE values[RTL_NUMBER_OF(whpx_register_names)];
};

struct whpx_nmi_trap {
    int pending;
    int trap, error_code, cr2;
};

struct whpx_vcpu {
#ifdef EMU_MICROSOFT
    WHV_EMULATOR_HANDLE emulator;
#endif
    bool window_registered;
    bool ready_for_pic_interrupt;
    unsigned long dirty;
    struct whpx_nmi_trap trap;
    bool interrupt_in_flight;

    critical_section irqreq_lock; /* protect cpu->interrupt_request */

    /* Must be the last field as it may have a tail */
    WHV_RUN_VP_EXIT_CONTEXT exit_ctx;
};

struct whpx_state whpx_global;

WHV_PARTITION_HANDLE whpx_get_partition(void)
{
    return whpx_global.partition;
}

/*
 * VP support
 */

static struct whpx_vcpu *whpx_vcpu(CPUState *cpu)
{
    return (struct whpx_vcpu *)cpu->hax_vcpu;
}

void
whpx_vcpu_irqreq_lock(CPUState *cpu)
{
    critical_section_enter(&whpx_vcpu(cpu)->irqreq_lock);
}

void
whpx_vcpu_irqreq_unlock(CPUState *cpu)
{
    critical_section_leave(&whpx_vcpu(cpu)->irqreq_lock);
}

int
whpx_inject_trap(int cpuidx, int trap, int error_code, int cr2)
{
    CPUState *cpu = whpx_get_cpu(cpuidx);
    struct whpx_vcpu *v;

    if (cpu) {
        v = whpx_vcpu(cpu);
        v->trap.trap = trap;
        v->trap.error_code = error_code;
        v->trap.cr2 = cr2;
        v->trap.pending = 1;

        whpx_vcpu_kick(cpu);

        return 0;
    }

    return -1;
}

void
apic_deliver_irq(
    uint8_t dest, uint8_t dest_mode, uint8_t delivery_mode,
    uint8_t vector_num, uint8_t trigger_mode)
{
    uint64_t t0 = 0;

    WHV_INTERRUPT_CONTROL interrupt = {
        .Type = delivery_mode, // Values correspond to delivery modes
        .DestinationMode = dest_mode ?
              WHvX64InterruptDestinationModeLogical
            : WHvX64InterruptDestinationModePhysical,
        .TriggerMode = trigger_mode ?
              WHvX64InterruptTriggerModeLevel
            : WHvX64InterruptTriggerModeEdge,
        .Vector = vector_num,
        .Destination = dest,
    };

    if (whpx_perf_stats)
        t0 = _rdtsc();

    HRESULT hr = WHvRequestInterrupt(whpx_get_partition(),
        &interrupt, sizeof(interrupt));
    if (FAILED(hr)) {
        debug_printf("whpx: IRQ request failed, delivery=%d destm=%d tm=%d "
            "vec=0x%x dest=%d, error %x\n",
            delivery_mode, dest_mode, trigger_mode, vector_num, dest, (int)hr);
    }

    if (whpx_perf_stats) {
        tmsum_request_irq += _rdtsc() - t0;
        count_request_irq++;
    }
}

static void whpx_registers_cpustate_to_hv(CPUState *cpu)
{
    struct CPUX86State *env = (CPUArchState *)(cpu->env_ptr);
    struct whpx_register_set vcxt = {};
    HRESULT hr;
    int idx = 0;
    int i;

    assert(cpu_is_stopped(cpu) || qemu_cpu_is_self(cpu));

    /* Indexes for first 16 registers match between HV and QEMU definitions */
    for (idx = 0; idx < CPU_NB_REGS64; idx += 1)
        vcxt.values[idx].Reg64 = env->regs[idx];

    /* Same goes for RIP and RFLAGS */
    assert(whpx_register_names[idx] == WHvX64RegisterRip);
    vcxt.values[idx++].Reg64 = env->eip;

    assert(whpx_register_names[idx] == WHvX64RegisterRflags);
    vcxt.values[idx++].Reg64 = env->eflags;

    /* Translate 6+4 segment registers. HV and QEMU order matches  */
    assert(idx == WHvX64RegisterEs);
    for (i = 0; i < 6; i += 1, idx += 1)
        vcxt.values[idx].Segment = whpx_seg_q2h(&env->segs[i]);

    assert(idx == WHvX64RegisterLdtr);
    vcxt.values[idx++].Segment = whpx_seg_q2h(&env->ldt);

    assert(idx == WHvX64RegisterTr);
    vcxt.values[idx++].Segment = whpx_seg_q2h(&env->tr);

    assert(idx == WHvX64RegisterIdtr);
    vcxt.values[idx].Table.Base = env->idt.base;
    vcxt.values[idx].Table.Limit = env->idt.limit;
    idx += 1;

    assert(idx == WHvX64RegisterGdtr);
    vcxt.values[idx].Table.Base = env->gdt.base;
    vcxt.values[idx].Table.Limit = env->gdt.limit;
    idx += 1;

    /* CR0, 2, 3, 4, 8 */
    assert(whpx_register_names[idx] == WHvX64RegisterCr0);
    vcxt.values[idx++].Reg64 = env->cr[0];
    assert(whpx_register_names[idx] == WHvX64RegisterCr2);
    vcxt.values[idx++].Reg64 = env->cr[2];
    assert(whpx_register_names[idx] == WHvX64RegisterCr3);
    vcxt.values[idx++].Reg64 = env->cr[3];
    assert(whpx_register_names[idx] == WHvX64RegisterCr4);
    vcxt.values[idx++].Reg64 = env->cr[4];

    /* 8 Debug Registers - Skipped */

    /* 16 XMM registers */
    assert(whpx_register_names[idx] == WHvX64RegisterXmm0);
    for (i = 0; i < 16; i += 1, idx += 1) {
        vcxt.values[idx].Reg128.Low64 = env->xmm_regs[i].ZMM_Q(0);
        vcxt.values[idx].Reg128.High64 = env->xmm_regs[i].ZMM_Q(1);
    }

    /* 8 FP registers */
    assert(whpx_register_names[idx] == WHvX64RegisterFpMmx0);
    for (i = 0; i < 8; i += 1, idx += 1) {
        vcxt.values[idx].Fp.AsUINT128.Low64 = env->fpregs[i].mmx.MMX_Q(0);
        /* vcxt.values[idx].Fp.AsUINT128.High64 =
               env->fpregs[i].mmx.MMX_Q(1);
        */
    }

    /* FP control status register */
    assert(whpx_register_names[idx] == WHvX64RegisterFpControlStatus);
    vcxt.values[idx].FpControlStatus.FpControl = env->fpuc;
    vcxt.values[idx].FpControlStatus.FpStatus =
        (env->fpus & ~0x3800) | (env->fpstt & 0x7) << 11;
    vcxt.values[idx].FpControlStatus.FpTag = 0;
    for (i = 0; i < 8; ++i) {
        vcxt.values[idx].FpControlStatus.FpTag |= (!env->fptags[i]) << i;
    }
    vcxt.values[idx].FpControlStatus.Reserved = 0;
    vcxt.values[idx].FpControlStatus.LastFpOp = env->fpop;
    vcxt.values[idx].FpControlStatus.LastFpRip = env->fpip;
    idx += 1;

    /* XMM control status register */
    assert(whpx_register_names[idx] == WHvX64RegisterXmmControlStatus);
    vcxt.values[idx].XmmControlStatus.LastFpRdp = 0;
    vcxt.values[idx].XmmControlStatus.XmmStatusControl = env->mxcsr;
    vcxt.values[idx].XmmControlStatus.XmmStatusControlMask = 0x0000ffff;
    idx += 1;

    /* MSRs */
    assert(whpx_register_names[idx] == WHvX64RegisterEfer);
    vcxt.values[idx++].Reg64 = env->efer;
#ifdef TARGET_X86_64
    assert(whpx_register_names[idx] == WHvX64RegisterKernelGsBase);
    vcxt.values[idx++].Reg64 = env->kernelgsbase;
#endif

    /* WHvX64RegisterPat - Skipped */

    assert(whpx_register_names[idx] == WHvX64RegisterSysenterCs);
    vcxt.values[idx++].Reg64 = env->sysenter_cs;
    assert(whpx_register_names[idx] == WHvX64RegisterSysenterEip);
    vcxt.values[idx++].Reg64 = env->sysenter_eip;
    assert(whpx_register_names[idx] == WHvX64RegisterSysenterEsp);
    vcxt.values[idx++].Reg64 = env->sysenter_esp;
    assert(whpx_register_names[idx] == WHvX64RegisterStar);
    vcxt.values[idx++].Reg64 = env->star;
#ifdef TARGET_X86_64
    assert(whpx_register_names[idx] == WHvX64RegisterLstar);
    vcxt.values[idx++].Reg64 = env->lstar;
    assert(whpx_register_names[idx] == WHvX64RegisterCstar);
    vcxt.values[idx++].Reg64 = env->cstar;
    assert(whpx_register_names[idx] == WHvX64RegisterSfmask);
    vcxt.values[idx++].Reg64 = env->fmask;
#endif

    /* Interrupt / Event Registers - Skipped */

    assert(idx == RTL_NUMBER_OF(whpx_register_names));

    hr = whpx_set_vp_registers(cpu->cpu_index,
        whpx_register_names,
        RTL_NUMBER_OF(whpx_register_names),
        &vcxt.values[0]);

    if (FAILED(hr))
        whpx_panic("WHPX: Failed to set virtual processor context, hr=%08lx",
            hr);

    return;
}

#ifndef EMU_MICROSOFT
static void
set_rax_and_ip(CPUState *cpu, uint64_t rax, uint64_t rip)
{
    WHV_REGISTER_NAME names[2] = { WHvX64RegisterRax, WHvX64RegisterRip };
    WHV_REGISTER_VALUE values[2];
    HRESULT hr;

    values[0].Reg64 = rax;
    values[1].Reg64 = rip;
    hr = whpx_set_vp_registers(cpu->cpu_index, names, 2, values);
    if (FAILED(hr))
        whpx_panic("failed to set registers: %lx\n", hr);
}

static void
set_ip(CPUState *cpu, uint64_t ip)
{
    WHV_REGISTER_NAME names[1] = { WHvX64RegisterRip };
    WHV_REGISTER_VALUE values[1];
    HRESULT hr;

    values[0].Reg64 = ip;
    hr = whpx_set_vp_registers(cpu->cpu_index, names, 1, values);
    if (FAILED(hr))
        whpx_panic("failed to set registers: %lx\n", hr);
}
#endif

int
whpx_cpu_has_work(CPUState *env)
{
    int work;

    whpx_vcpu_irqreq_lock(env);
    work = ((env->interrupt_request & (CPU_INTERRUPT_HARD |
                                      CPU_INTERRUPT_POLL)) &&
            (env->eflags & IF_MASK)) ||
           (env->interrupt_request & (CPU_INTERRUPT_NMI |
                                     CPU_INTERRUPT_INIT |
                                     CPU_INTERRUPT_SIPI |
                                     CPU_INTERRUPT_MCE)) ||
           ((env->interrupt_request & CPU_INTERRUPT_SMI) &&
            !(env->hflags & HF_SMM_MASK));
    whpx_vcpu_irqreq_unlock(env);

    return work;
}

static void whpx_registers_hv_to_cpustate(CPUState *cpu)
{
    struct CPUX86State *env = (CPUArchState *)(cpu->env_ptr);
    struct whpx_register_set vcxt;
    HRESULT hr;
    int idx = 0;
    int i;

    assert(cpu_is_stopped(cpu) || qemu_cpu_is_self(cpu));

    hr = whpx_get_vp_registers(cpu->cpu_index,
        whpx_register_names,
        RTL_NUMBER_OF(whpx_register_names),
        &vcxt.values[0]);

    if (FAILED(hr))
        whpx_panic("WHPX: Failed to get virtual processor context, hr=%08lx",
            hr);

    /* Indexes for first 16 registers match between HV and QEMU definitions */
    for (idx = 0; idx < CPU_NB_REGS64; idx += 1)
        env->regs[idx] = vcxt.values[idx].Reg64;

    /* Same goes for RIP and RFLAGS */
    assert(whpx_register_names[idx] == WHvX64RegisterRip);
    env->eip = vcxt.values[idx++].Reg64;
    assert(whpx_register_names[idx] == WHvX64RegisterRflags);
    env->eflags = vcxt.values[idx++].Reg64;

    /* Translate 6+4 segment registers. HV and QEMU order matches  */
    assert(idx == WHvX64RegisterEs);
    for (i = 0; i < 6; i += 1, idx += 1)
        env->segs[i] = whpx_seg_h2q(&vcxt.values[idx].Segment);

    assert(idx == WHvX64RegisterLdtr);
    env->ldt = whpx_seg_h2q(&vcxt.values[idx++].Segment);
    assert(idx == WHvX64RegisterTr);
    env->tr = whpx_seg_h2q(&vcxt.values[idx++].Segment);
    assert(idx == WHvX64RegisterIdtr);
    env->idt.base = vcxt.values[idx].Table.Base;
    env->idt.limit = vcxt.values[idx].Table.Limit;
    idx += 1;
    assert(idx == WHvX64RegisterGdtr);
    env->gdt.base = vcxt.values[idx].Table.Base;
    env->gdt.limit = vcxt.values[idx].Table.Limit;
    idx += 1;

    /* CR0, 2, 3, 4, 8 */
    assert(whpx_register_names[idx] == WHvX64RegisterCr0);
    env->cr[0] = vcxt.values[idx++].Reg64;
    assert(whpx_register_names[idx] == WHvX64RegisterCr2);
    env->cr[2] = vcxt.values[idx++].Reg64;
    assert(whpx_register_names[idx] == WHvX64RegisterCr3);
    env->cr[3] = vcxt.values[idx++].Reg64;
    assert(whpx_register_names[idx] == WHvX64RegisterCr4);
    env->cr[4] = vcxt.values[idx++].Reg64;

    /* 8 Debug Registers - Skipped */

    /* 16 XMM registers */
    assert(whpx_register_names[idx] == WHvX64RegisterXmm0);
    for (i = 0; i < 16; i += 1, idx += 1) {
        env->xmm_regs[i].ZMM_Q(0) = vcxt.values[idx].Reg128.Low64;
        env->xmm_regs[i].ZMM_Q(1) = vcxt.values[idx].Reg128.High64;
    }

    /* 8 FP registers */
    assert(whpx_register_names[idx] == WHvX64RegisterFpMmx0);
    for (i = 0; i < 8; i += 1, idx += 1) {
        env->fpregs[i].mmx.MMX_Q(0) = vcxt.values[idx].Fp.AsUINT128.Low64;
        /* env->fpregs[i].mmx.MMX_Q(1) =
               vcxt.values[idx].Fp.AsUINT128.High64;
        */
    }

    /* FP control status register */
    assert(whpx_register_names[idx] == WHvX64RegisterFpControlStatus);
    env->fpuc = vcxt.values[idx].FpControlStatus.FpControl;
    env->fpstt = (vcxt.values[idx].FpControlStatus.FpStatus >> 11) & 0x7;
    env->fpus = vcxt.values[idx].FpControlStatus.FpStatus & ~0x3800;
    for (i = 0; i < 8; ++i) {
        env->fptags[i] = !((vcxt.values[idx].FpControlStatus.FpTag >> i) & 1);
    }
    env->fpop = vcxt.values[idx].FpControlStatus.LastFpOp;
    env->fpip = vcxt.values[idx].FpControlStatus.LastFpRip;
    idx += 1;

    /* XMM control status register */
    assert(whpx_register_names[idx] == WHvX64RegisterXmmControlStatus);
    env->mxcsr = vcxt.values[idx].XmmControlStatus.XmmStatusControl;
    idx += 1;

    /* MSRs */
    assert(whpx_register_names[idx] == WHvX64RegisterEfer);
    env->efer = vcxt.values[idx++].Reg64;
#ifdef TARGET_X86_64
    assert(whpx_register_names[idx] == WHvX64RegisterKernelGsBase);
    env->kernelgsbase = vcxt.values[idx++].Reg64;
#endif

    /* WHvX64RegisterPat - Skipped */

    assert(whpx_register_names[idx] == WHvX64RegisterSysenterCs);
    env->sysenter_cs = vcxt.values[idx++].Reg64;;
    assert(whpx_register_names[idx] == WHvX64RegisterSysenterEip);
    env->sysenter_eip = vcxt.values[idx++].Reg64;
    assert(whpx_register_names[idx] == WHvX64RegisterSysenterEsp);
    env->sysenter_esp = vcxt.values[idx++].Reg64;
    assert(whpx_register_names[idx] == WHvX64RegisterStar);
    env->star = vcxt.values[idx++].Reg64;
#ifdef TARGET_X86_64
    assert(whpx_register_names[idx] == WHvX64RegisterLstar);
    env->lstar = vcxt.values[idx++].Reg64;
    assert(whpx_register_names[idx] == WHvX64RegisterCstar);
    env->cstar = vcxt.values[idx++].Reg64;
    assert(whpx_register_names[idx] == WHvX64RegisterSfmask);
    env->fmask = vcxt.values[idx++].Reg64;
#endif

    /* Interrupt / Event Registers - Skipped */

    assert(idx == RTL_NUMBER_OF(whpx_register_names));

    return;
}

#ifdef EMU_MICROSOFT
static HRESULT CALLBACK whpx_emu_ioport_callback(
    void *ctx,
    WHV_EMULATOR_IO_ACCESS_INFO *IoAccess)
{
    int w = ioport_width(IoAccess->AccessSize, "%s:%d: bad width",
        __FUNCTION__, __LINE__);
    uint32_t *v = (void*)&IoAccess->Data;

#ifdef DEBUG_IOPORT
    if (IoAccess->Direction && IoAccess->Port != DEBUG_PORT_NUMBER) //debug port
        debug_printf("IOPORT %5s w=%d port=0x%04x value=0x%08x\n",
            IoAccess->Direction ? "write":"read",
            IoAccess->AccessSize, IoAccess->Port, IoAccess->Data);
#endif
    if (IoAccess->Direction == 0) /* in */
        *v = ioport_read(w, IoAccess->Port);
    else /* out */
        ioport_write(w, IoAccess->Port, *v);
#ifdef DEBUG_IOPORT
    if (!IoAccess->Direction && IoAccess->Port != DEBUG_PORT_NUMBER)
        debug_printf("IOPORT %5s w=%d port=0x%04x value=0x%08x\n",
            IoAccess->Direction ? "write":"read",
            IoAccess->AccessSize, IoAccess->Port, IoAccess->Data);
#endif
    return S_OK;
}

static HRESULT CALLBACK whpx_emu_memio_callback(
    void *ctx,
    WHV_EMULATOR_MEMORY_ACCESS_INFO *ma)
{
#ifdef DEBUG_MMIO
    if (ma->Direction) {
        uint64_t v = 0;
        switch (ma->AccessSize) {
        case 1: v = *((uint8_t*)ma->Data); break;
        case 2: v = *((uint16_t*)ma->Data); break;
        case 4: v = *((uint32_t*)ma->Data); break;
        case 8: v = *((uint64_t*)ma->Data); break;
        default: whpx_panic("bad access size %d\n", ma->AccessSize); break;
        }
        debug_printf("MMIO   %5s address=%016"PRIx64" size=%d value=%08"PRIx64
            "\n",
            ma->Direction ? "write":"read", ma->GpaAddress, ma->AccessSize,
            v);
    }
#endif
    cpu_physical_memory_rw(ma->GpaAddress, ma->Data, ma->AccessSize,
                           ma->Direction);
#ifdef DEBUG_MMIO
    if (!ma->Direction) {
        uint64_t v = 0;
        switch (ma->AccessSize) {
        case 1: v = *((uint8_t*)ma->Data); break;
        case 2: v = *((uint16_t*)ma->Data); break;
        case 4: v = *((uint32_t*)ma->Data); break;
        case 8: v = *((uint64_t*)ma->Data); break;
        default: whpx_panic("bad access size %d\n", ma->AccessSize); break;
        }
        debug_printf("MMIO   %5s address=%016"PRIx64" size=%d value=%08"PRIx64
            "\n",
            ma->Direction ? "write":"read", ma->GpaAddress, ma->AccessSize,
            v);
    }
#endif
    return S_OK;
}

static HRESULT CALLBACK whpx_emu_getreg_callback(
    void *ctx,
    const WHV_REGISTER_NAME *RegisterNames,
    UINT32 RegisterCount,
    WHV_REGISTER_VALUE *RegisterValues)
{
    HRESULT hr;
    CPUState *cpu = (CPUState *)ctx;

    hr = whpx_get_vp_registers(cpu->cpu_index, RegisterNames, RegisterCount,
        RegisterValues);
    if (FAILED(hr))
        whpx_panic("WHPX: Failed to get virtual processor registers,"
            " hr=%08lx", hr);

    return hr;
}

static HRESULT CALLBACK whpx_emu_setreg_callback(
    void *ctx,
    const WHV_REGISTER_NAME *RegisterNames,
    UINT32 RegisterCount,
    const WHV_REGISTER_VALUE *RegisterValues)
{
    HRESULT hr;
    CPUState *cpu = (CPUState *)ctx;

    hr = whpx_set_vp_registers(cpu->cpu_index, RegisterNames, RegisterCount,
        RegisterValues);
    if (FAILED(hr))
        whpx_panic("WHPX: Failed to set virtual processor registers,"
            " hr=%08lx", hr);

    /*
     * The emulator just successfully wrote the register state. We clear the
     * dirty state so we avoid the double write on resume of the VP.
     */
    whpx_vcpu(cpu)->dirty &= ~VCPU_DIRTY_CPUSTATE;

    return hr;
}

static HRESULT CALLBACK whpx_emu_translate_callback(
    void *ctx,
    WHV_GUEST_VIRTUAL_ADDRESS Gva,
    WHV_TRANSLATE_GVA_FLAGS TranslateFlags,
    WHV_TRANSLATE_GVA_RESULT_CODE *TranslationResult,
    WHV_GUEST_PHYSICAL_ADDRESS *Gpa)
{
    HRESULT hr;
    struct whpx_state *whpx = &whpx_global;
    CPUState *cpu = (CPUState *)ctx;
    WHV_TRANSLATE_GVA_RESULT res;

    hr = WHvTranslateGva(whpx->partition, cpu->cpu_index,
                         Gva, TranslateFlags, &res, Gpa);
    if (FAILED(hr)) {
        whpx_panic("WHPX: Failed to translate GVA, hr=%08lx", hr);
    } else {
        *TranslationResult = res.ResultCode;
    }

    return hr;
}

static const WHV_EMULATOR_CALLBACKS whpx_emu_callbacks = {
    .Size = sizeof(WHV_EMULATOR_CALLBACKS),
    .WHvEmulatorIoPortCallback = whpx_emu_ioport_callback,
    .WHvEmulatorMemoryCallback = whpx_emu_memio_callback,
    .WHvEmulatorGetVirtualProcessorRegisters = whpx_emu_getreg_callback,
    .WHvEmulatorSetVirtualProcessorRegisters = whpx_emu_setreg_callback,
    .WHvEmulatorTranslateGvaPage = whpx_emu_translate_callback,
};
#endif

#ifndef EMU_MICROSOFT
static void
whpx_vcpu_fetch_emulation_registers(CPUState *cpu)
{
    WHV_REGISTER_VALUE reg_values[WHPX_MAX_REGISTERS];
    HRESULT hr;
    whpx_reg_list_t *regs = emu_get_read_registers();

    hr = whpx_get_vp_registers(cpu->cpu_index, &regs->reg[0],
        regs->num, reg_values);
    if (FAILED(hr))
        whpx_panic("WHPX: Failed to get emu registers,"
            " hr=%08lx", hr);
    emu_registers_hv_to_cpustate(cpu, reg_values);
}
#endif


static int
whpx_handle_mmio(CPUState *cpu, WHV_MEMORY_ACCESS_CONTEXT *ctx)
{
#ifndef EMU_MICROSOFT
    whpx_vcpu_fetch_emulation_registers(cpu);
    whpx_lock_iothread();
    if (ctx->InstructionByteCount)
        emu_one(cpu, ctx->InstructionBytes, ctx->InstructionByteCount);
    else
        emu_one(cpu, NULL, 0);
    whpx_unlock_iothread();
    /* emulator dirtied CPUState */
    whpx_vcpu(cpu)->dirty |= VCPU_DIRTY_EMU;
#else
    HRESULT hr;
    struct whpx_vcpu *vcpu = whpx_vcpu(cpu);
    WHV_EMULATOR_STATUS emu_status;

    whpx_lock_iothread();
    hr = WHvEmulatorTryMmioEmulation(vcpu->emulator, cpu,
        &vcpu->exit_ctx.VpContext, ctx, &emu_status);
    whpx_unlock_iothread();
    if (FAILED(hr))
        whpx_panic("WHPX: Failed to parse MMIO access, hr=%08lx", hr);

    if (!emu_status.EmulationSuccessful)
        whpx_panic("WHPX: Failed to emulate MMIO access");

#endif
    return 0;
}

#ifndef EMU_MICROSOFT
static int
try_simple_portio(CPUState *cpu, WHV_X64_IO_PORT_ACCESS_CONTEXT *ctx)
{
    WHV_X64_IO_PORT_ACCESS_INFO *access = &ctx->AccessInfo;
    struct whpx_vcpu *vcpu = whpx_vcpu(cpu);
    int port = ctx->PortNumber;
    int instrlen;

    if (!port || access->RepPrefix || access->StringOp)
        return -1;

    instrlen = vcpu->exit_ctx.VpContext.InstructionLength;
    assert(instrlen);

    if (access->IsWrite) {
        whpx_lock_iothread();
        emu_simple_port_io(1, port, access->AccessSize, &ctx->Rax);
        whpx_unlock_iothread();

        cpu->eip += instrlen;
        set_ip(cpu, cpu->eip);
    } else {
        uint64_t rax = ctx->Rax;

        whpx_lock_iothread();
        emu_simple_port_io(0, port, access->AccessSize, &rax);
        whpx_unlock_iothread();

        cpu->eip += instrlen;
        set_rax_and_ip(cpu, rax, cpu->eip);
    }

    return 0;
}
#endif

static int
whpx_handle_portio(CPUState *cpu, WHV_X64_IO_PORT_ACCESS_CONTEXT *ctx)
{
#ifndef EMU_MICROSOFT
    /* perhaps can use HyperV forwarded ioport access data for quicker
       emulation */
    if (try_simple_portio(cpu, ctx) != 0) {
        whpx_vcpu_fetch_emulation_registers(cpu);
        /* full emu path */
        whpx_lock_iothread();
        if (ctx->InstructionByteCount)
            emu_one(cpu, ctx->InstructionBytes, ctx->InstructionByteCount);
        else
            emu_one(cpu, NULL, 0);
        whpx_unlock_iothread();
        /* emulator dirtied CPUState */
        whpx_vcpu(cpu)->dirty |= VCPU_DIRTY_EMU;
    }
#else
    HRESULT hr;
    struct whpx_vcpu *vcpu = whpx_vcpu(cpu);
    WHV_EMULATOR_STATUS emu_status;

    whpx_lock_iothread();
    hr = WHvEmulatorTryIoEmulation(vcpu->emulator, cpu,
        &vcpu->exit_ctx.VpContext, ctx, &emu_status);
    whpx_unlock_iothread();
    if (FAILED(hr))
        whpx_panic("WHPX: Failed to parse PortIO access, hr=%08lx", hr);

    if (!emu_status.EmulationSuccessful)
        whpx_panic("WHPX: Failed to emulate PortMMIO access");
#endif
    return 0;
}

static int
whpx_handle_halt(CPUState *cpu)
{
    int ret = 0;

    // should not happen with apic virt
    assert(false);

    if (!whpx_cpu_has_work(cpu)) {
        cpu->exception_index = EXCP_HLT;
        cpu->halted = true;
        ret = 1;
    }

    return ret;
}

static int
whpx_handle_msr_read(CPUState *cpu, uint32_t msr, uint64_t *content)
{
    int handled = 1;

    switch (msr) {
    default:
        handled = rdmsr_viridian_regs(msr, content);
        if (handled)
            break;
        debug_printf("unhandled MSR[0x%x] read\n", msr);
    }

    return handled;
}

static int
whpx_handle_msr_write(CPUState *cpu, uint32_t msr, uint64_t content)
{
    int handled = 1;

    switch (msr) {
    default:
        handled = wrmsr_viridian_regs(msr, content);
        if (handled)
            break;
        debug_printf("unhandled MSR[0x%x] write = %"PRIx64"\n", msr, content);
    }

    return handled;
}

static int
whpx_handle_msr_access(CPUState *cpu)
{
    struct whpx_vcpu *vcpu = whpx_vcpu(cpu);
    WHV_X64_MSR_ACCESS_CONTEXT *msr = &vcpu->exit_ctx.MsrAccess;
    WHV_REGISTER_NAME reg[3] = {
        WHvX64RegisterRip,
        WHvX64RegisterRdx,
        WHvX64RegisterRax
    };
    WHV_REGISTER_VALUE val[3];
    uint32_t msr_index = msr->MsrNumber;
    uint64_t msr_content = 0;
    int num_write_regs = 0;
    HRESULT hr;

    if (msr->AccessInfo.IsWrite) {
        msr_content = ((uint64_t)msr->Rdx << 32) | (uint32_t)msr->Rax;
        whpx_handle_msr_write(cpu, msr_index, msr_content);
        num_write_regs = 1;
    } else {
        whpx_handle_msr_read(cpu, msr_index, &msr_content);
        /* rdx */
        val[1].Reg64 = msr_content >> 32;
        /* rax */
        val[2].Reg64 = msr_content & 0xFFFFFFFF;
        num_write_regs = 3;
    }

    /* rip */
    val[0].Reg64 = vcpu->exit_ctx.VpContext.Rip +
        vcpu->exit_ctx.VpContext.InstructionLength;

    hr = whpx_set_vp_registers(cpu->cpu_index,
        reg, num_write_regs, &val[0]);
    if (FAILED(hr))
        whpx_panic("WHPX: Failed to set registers, hr=%08lx", hr);

    return 0;
}

extern int do_v4v_op_cpuid(CPUState *cpu,
    uint64_t rdi, uint64_t rsi, uint64_t rdx, uint64_t r10, uint64_t r9, uint64_t r8);

int
do_memory_op_cpuid(CPUState *cpu, uint64_t rdi, uint64_t rsi)
{
    uint64_t cmd = rdi;

    switch (cmd) {
    case WHPXMEM_share_zero_pages: {
        struct whpx_memory_share_zero_pages sh = { 0 };
        whpx_copy_from_guest_va(cpu, &sh, rsi, sizeof(sh));
        uint64_t len = sh.nr_gpfns * sizeof(uint64_t);
        uint64_t *pfns = whpx_ram_map(sh.gpfn_list_gpfn << PAGE_SHIFT, &len);
        assert(pfns);
        whpx_memory_balloon_grow(sh.nr_gpfns, pfns);
        whpx_ram_unmap(pfns);
        break;
    }
    default:
        return -ENOSYS;
    }

    return 0;
}

static int
cpuid_viridian_hypercall(uint64_t leaf, uint64_t *eax,
    uint64_t *ebx, uint64_t *ecx,
    uint64_t *edx)
{
    /* viridian hypercalls are done with cpuid, leaf marked with bits 30+31 */
    leaf &= 0xFFFFFFFF;
    if (!((leaf & 0xC0000000) == 0xC0000000))
        return 0;

    viridian_hypercall(eax);

    return 1;
}

static uint64_t
cpuid_hypervisor_base_leaf(void)
{
    return vm_viridian ? 0x40000100 : 0x40000000;
}

static int
cpuid_hypervisor(uint64_t leaf, uint64_t *rax,
    uint64_t *rbx, uint64_t *rcx,
    uint64_t *rdx)
{
    leaf -= cpuid_hypervisor_base_leaf();

    switch (leaf) {
    case  1:
        *rax = 0; /* version number */
        *rbx = 0;
        *rcx = 0;
        *rdx = 0;
        return 1;
    case 2:
        *rax = 0;
        *rbx = 0;
        *rcx = 0;
        *rdx = 0;
        return 1;
    case 192:
        *rax = whpx_global.seed_lo & 0xffffffff;
        *rbx = whpx_global.seed_lo >> 32;
        *rcx = whpx_global.seed_hi & 0xffffffff;
        *rdx = whpx_global.seed_hi >> 32;
        return 1;
    case 193:
        *rax = whpx_global.dm_features;
        return 1;
    default:
        return 0;
    }
}

static int
whpx_handle_cpuid(CPUState *cpu)
{
#define CPUID_REGS_NUM_READ  14
#define CPUID_REGS_NUM_WRITE 10

    struct whpx_vcpu *vcpu = whpx_vcpu(cpu);
    WHV_X64_CPUID_ACCESS_CONTEXT *cpuid = &vcpu->exit_ctx.CpuidAccess;
    WHV_REGISTER_NAME regs[CPUID_REGS_NUM_READ] = {
        /* read/write these regs */
        WHvX64RegisterRax,
        WHvX64RegisterRcx,
        WHvX64RegisterRdx,
        WHvX64RegisterRbx,
        WHvX64RegisterRip,
        WHvX64RegisterRdi,
        WHvX64RegisterRsi,
        WHvX64RegisterR8,
        WHvX64RegisterR9,
        WHvX64RegisterR10,

        /* only read these regs */
        WHvX64RegisterCr0,
        WHvX64RegisterCr3,
        WHvX64RegisterCr4,
        WHvX64RegisterEfer,
    };
    WHV_REGISTER_VALUE values[CPUID_REGS_NUM_READ];
    uint64_t rax, rcx, rdx, rbx, rdi, rsi, r8, r9, r10;
    HRESULT hr;

    hr = whpx_get_vp_registers(cpu->cpu_index,
        regs, CPUID_REGS_NUM_READ, &values[0]);
    if (FAILED(hr))
        whpx_panic("WHPX: Failed to access registers, hr=%08lx", hr);

    rax = values[0].Reg64;
    rcx = values[1].Reg64;
    rdx = values[2].Reg64;
    rbx = values[3].Reg64;
    rdi = values[5].Reg64;
    rsi = values[6].Reg64;
    r8  = values[7].Reg64;
    r9  = values[8].Reg64;
    r10 = values[9].Reg64;

    switch (rax) {
    case 1:
        rax = cpuid->DefaultResultRax;
        rcx = cpuid->DefaultResultRcx;
        rdx = cpuid->DefaultResultRdx;
        rbx = cpuid->DefaultResultRbx;
        rcx |= CPUID_EXT_HYPERVISOR;
        break;
    case 0x40000000:
        if (vm_viridian) {
            cpuid_viridian_leaves(rax, &rax, &rbx, &rcx, &rdx);
            break;
        };
        /* fall through */
    case 0x40000100:
        rax = cpuid_hypervisor_base_leaf() + 2;
        rcx = WHP_CPUID_SIGNATURE_ECX;
        rdx = WHP_CPUID_SIGNATURE_EDX;
        rbx = WHP_CPUID_SIGNATURE_EBX;
        break;
    case 0x40000001 ... 0x40000006:
        cpuid_viridian_leaves(rax, &rax, &rbx, &rcx, &rdx);
        break;
    case CPUID_DEBUG_OUT_8:
        whpx_debug_char((char)rcx);
        break;
    case CPUID_DEBUG_OUT_32:
        whpx_debug_char((char)rcx & 0xff);
        whpx_debug_char((char)((rcx & 0xff00) >> 8));
        whpx_debug_char((char)((rcx & 0xff0000) >> 16));
        whpx_debug_char((char)((rcx & 0xff000000) >> 24));
        break;
    case __WHPX_HYPERVISOR_memory_op: {
        rax = do_memory_op_cpuid(cpu, rdi, rsi);
        break;
    }
    case __WHPX_HYPERVISOR_v4v_op: {
        uint64_t t0 = 0;

        if (whpx_perf_stats)
            t0 = _rdtsc();

        /* update paging related registers - v4v will need to resolve
         * virtual addrs (whpx_translate_gva_to_gpa) */
        cpu->cr[0] = values[10].Reg64;
        cpu->cr[3] = values[11].Reg64;
        cpu->cr[4] = values[12].Reg64;
        cpu->efer  = values[13].Reg64;

        /* handle v4v op */
        rax = do_v4v_op_cpuid(cpu, rdi, rsi, rdx, r10, r8, r9);

        if (whpx_perf_stats) {
            tmsum_v4v += _rdtsc() - t0;
            count_v4v++;
        }
        break;
    }
    default:
        /* check if this is hypervisor cpuid */
        if (cpuid_hypervisor(rax, &rax, &rbx, &rcx, &rdx))
            break;

        /* check if the cpuid was viridian hypercall, since we setup
         * viridian hypercall page to invoke calls via cpuid */
        if (!cpuid_viridian_hypercall(rax, &rax, &rbx, &rcx, &rdx)) {
            rax = cpuid->DefaultResultRax;
            rcx = cpuid->DefaultResultRcx;
            rdx = cpuid->DefaultResultRdx;
            rbx = cpuid->DefaultResultRbx;
        }
        break;
    }

    values[0].Reg64 = rax;
    values[1].Reg64 = rcx;
    values[2].Reg64 = rdx;
    values[3].Reg64 = rbx;
    values[4].Reg64 = vcpu->exit_ctx.VpContext.Rip +
        vcpu->exit_ctx.VpContext.InstructionLength;
    values[5].Reg64 = rdi;
    values[6].Reg64 = rsi;
    values[7].Reg64 = r8;
    values[8].Reg64 = r9;
    values[9].Reg64 = r10;

    hr = whpx_set_vp_registers(cpu->cpu_index,
        regs, CPUID_REGS_NUM_WRITE, &values[0]);
    if (FAILED(hr))
        whpx_panic("WHPX: Failed to set registers, hr=%08lx", hr);

    return 0;
}

void
whpx_vcpu_flush_dirty(CPUState *cpu)
{
    struct whpx_vcpu *vcpu = whpx_vcpu(cpu);

    /* hyper-v state shouldn't be dirtied at the same time as CPUState */
    assert(! ((vcpu->dirty & VCPU_DIRTY_CPUSTATE) &&
              (vcpu->dirty & VCPU_DIRTY_HV)));
    assert(! ((vcpu->dirty & VCPU_DIRTY_EMU) &&
              (vcpu->dirty & VCPU_DIRTY_HV)));

    if (vcpu->dirty & VCPU_DIRTY_HV) {
        whpx_registers_hv_to_cpustate(cpu);
        vcpu->dirty &= ~VCPU_DIRTY_HV;
    }

    if (vcpu->dirty & VCPU_DIRTY_CPUSTATE) {
        whpx_registers_cpustate_to_hv(cpu);
        vcpu->dirty &= ~VCPU_DIRTY_CPUSTATE;
        vcpu->dirty &= ~VCPU_DIRTY_EMU;
    }

    if (vcpu->dirty & VCPU_DIRTY_EMU) {
        WHV_REGISTER_NAME reg_names[WHPX_MAX_REGISTERS];
        WHV_REGISTER_VALUE reg_values[WHPX_MAX_REGISTERS];

        int num = emu_registers_cpustate_to_hv(cpu, WHPX_MAX_REGISTERS,
            &reg_names[0], &reg_values[0]);
        HRESULT hr = whpx_set_vp_registers(cpu->cpu_index, reg_names,
            num, reg_values);
        if (FAILED(hr))
            whpx_panic("failed to set emu registers\n");
        vcpu->dirty &= ~VCPU_DIRTY_EMU;
    }
}

static void
whpx_vcpu_pre_run(CPUState *cpu)
{
    HRESULT hr;
    struct whpx_vcpu *vcpu = whpx_vcpu(cpu);
    struct CPUX86State *env = (CPUArchState *)(cpu->env_ptr);
    int irq;
    WHV_X64_PENDING_INTERRUPTION_REGISTER new_int = {};
    WHV_X64_PENDING_EXT_INT_EVENT new_ext_int = {};
    UINT32 reg_count = 0;
    WHV_REGISTER_VALUE reg_values[4] = { };
    WHV_REGISTER_NAME reg_names[4];

    whpx_vcpu_irqreq_lock(cpu);

    /* Inject user trap */
    if (!vcpu->interrupt_in_flight && vcpu->trap.pending) {
        vcpu->trap.pending = false;
        vcpu->ready_for_pic_interrupt = false;
        new_int.InterruptionType = WHvX64PendingNmi;
        new_int.InterruptionPending = 1;
        new_int.InterruptionVector = vcpu->trap.trap;
        if (vcpu->trap.error_code != -1) {
            new_int.ErrorCode = vcpu->trap.error_code;
            new_int.DeliverErrorCode = 1;
        }
    }

    /* Inject PIC interruption */
    if (vcpu->ready_for_pic_interrupt &&
        (cpu->interrupt_request & CPU_INTERRUPT_HARD)) {
        cpu->interrupt_request &= ~CPU_INTERRUPT_HARD;
        whpx_vcpu_irqreq_unlock(cpu);
        whpx_lock_iothread();
        irq = cpu_get_pic_interrupt(env);
        whpx_unlock_iothread();
        whpx_vcpu_irqreq_lock(cpu);

        if (irq >= 0) {
            new_ext_int = (WHV_X64_PENDING_EXT_INT_EVENT){
                .EventPending = 1,
                .EventType = WHvX64PendingEventExtInt,
                .Vector = irq,
            };
#if 0
            viridian_synic_ack_irq(env, irq);
#endif
        }
    }

    /* Setup interrupt state if new one was prepared */

    /* raw inject */
    if (new_int.InterruptionPending) {
        reg_names[reg_count] = WHvRegisterPendingInterruption;
        reg_values[reg_count].PendingInterruption = new_int;
        reg_count++;
    }

    /* apic ext int inject */
    if (new_ext_int.EventPending) {
        reg_names[reg_count] = WHvRegisterPendingEvent;
        reg_values[reg_count].ExtIntEvent = new_ext_int;
        reg_count++;
    }

    /* Update the state of the interrupt delivery notification */
    if (!vcpu->window_registered &&
        (cpu->interrupt_request & CPU_INTERRUPT_HARD)) {
        reg_names[reg_count] = WHvX64RegisterDeliverabilityNotifications;
        reg_values[reg_count].DeliverabilityNotifications.InterruptNotification = 1;
        reg_count++;
        vcpu->window_registered = 1;
    }

    whpx_vcpu_irqreq_unlock(cpu);
    vcpu->ready_for_pic_interrupt = false;

    if (reg_count) {
        hr = whpx_set_vp_registers(cpu->cpu_index, reg_names,
            reg_count, reg_values);
        if (FAILED(hr)) {
            whpx_dump_cpu_state(cpu->cpu_index);
            debug_printf("TRIED TO SET:\n");
            dump_whv_register_list(reg_names, reg_values, reg_count);
            whpx_panic("WHPX: Failed to set vp registers,"
                " hr=%08lx", hr);
        }
    }
}

static void
whpx_vcpu_post_run(CPUState *cpu)
{
    struct whpx_vcpu *vcpu = whpx_vcpu(cpu);
    struct CPUX86State *env = (CPUArchState *)(cpu->env_ptr);
    WHV_VP_EXIT_CONTEXT *vp_ctx = &vcpu->exit_ctx.VpContext;

    env->eip = vp_ctx->Rip;
    env->eflags = vp_ctx->Rflags;
    vcpu->interrupt_in_flight = vp_ctx->ExecutionState.InterruptionPending;
}

static int
whpx_vcpu_run(CPUState *cpu)
{
    HRESULT hr;
    struct whpx_state *whpx = &whpx_global;
    struct whpx_vcpu *vcpu = whpx_vcpu(cpu);
    int ret = 0;
    uint64_t t0 = 0;

    whpx_vcpu_flush_dirty(cpu);

    do {
        whpx_vcpu_pre_run(cpu);

#ifdef DEBUG_CPU
        whpx_dump_cpu_state(cpu->cpu_index);
#endif
        if (whpx_perf_stats)
            t0 = _rdtsc();
        hr = WHvRunVirtualProcessor(whpx->partition, cpu->cpu_index,
            &vcpu->exit_ctx, sizeof(vcpu->exit_ctx));
        if (whpx_perf_stats) {
            tmsum_runvp += _rdtsc() - t0;
            count_runvp++;
        }

        if (FAILED(hr))
            whpx_panic("WHPX: Failed to exec a virtual processor,"
                " hr=%08lx", hr);
#ifdef DEBUG_CPU
        debug_printf("vcpu%d EXIT REASON %d\n",
            cpu->cpu_index, vcpu->exit_ctx.ExitReason);
        whpx_dump_cpu_state(cpu->cpu_index);
#endif
        whpx_vcpu_post_run(cpu);

        if (whpx_perf_stats)
            t0 = _rdtsc();

        switch (vcpu->exit_ctx.ExitReason) {
        case WHvRunVpExitReasonMemoryAccess:
            ret = whpx_handle_mmio(cpu, &vcpu->exit_ctx.MemoryAccess);
            break;

        case WHvRunVpExitReasonX64IoPortAccess:
            ret = whpx_handle_portio(cpu, &vcpu->exit_ctx.IoPortAccess);
            break;

        case WHvRunVpExitReasonX64InterruptWindow:
            vcpu->ready_for_pic_interrupt = true;
            vcpu->window_registered = 0;
            break;

        case WHvRunVpExitReasonX64ApicEoi:
            ioapic_eoi_broadcast(vcpu->exit_ctx.ApicEoi.InterruptVector);
            break;

        case WHvRunVpExitReasonX64Halt:
            debug_printf("VCPU%d HALT!\n", cpu->cpu_index);
            ret = whpx_handle_halt(cpu);
            break;

        case WHvRunVpExitReasonCanceled:
            cpu->exception_index = EXCP_INTERRUPT;
            ret = 1;
            break;

        case WHvRunVpExitReasonX64MsrAccess:
            ret = whpx_handle_msr_access(cpu);
            break;

        case WHvRunVpExitReasonX64Cpuid:
            ret = whpx_handle_cpuid(cpu);
            break;

        case WHvRunVpExitReasonNone:
        case WHvRunVpExitReasonUnrecoverableException:
        case WHvRunVpExitReasonInvalidVpRegisterValue:
        case WHvRunVpExitReasonUnsupportedFeature:
        case WHvRunVpExitReasonException:
        default: {
            uint64_t phys_rip = 0;
            int unmapped = 0;
            whpx_dump_cpu_state(cpu->cpu_index);
            whpx_translate_gva_to_gpa(cpu, 0, vcpu->exit_ctx.VpContext.Rip, &phys_rip,
                &unmapped);
            debug_printf("WHPX: Unexpected VP exit code %d @ phys-rip=%"PRIx64"\n",
                vcpu->exit_ctx.ExitReason, phys_rip);
            dump_phys_mem(phys_rip - 16, 32);
            assert(false);
            break;
        }
        }

        if (whpx_perf_stats) {
            uint8_t er = whpx_er_byte_encode(vcpu->exit_ctx.ExitReason);
            tmsum_vmexit[er] += _rdtsc() - t0;
            count_vmexit[er]++;
        }
        whpx_vcpu_flush_dirty(cpu);

        if (cpu->interrupt_request)
            ret = 1;

    } while (!ret);

    return ret < 0;
}

/*
 * Vcpu support.
 */

int whpx_init_vcpu(CPUState *cpu)
{
    HRESULT hr;
    struct whpx_state *whpx = &whpx_global;
    struct whpx_vcpu *vcpu;

    vcpu = g_malloc0(sizeof(struct whpx_vcpu));

    if (!vcpu) {
        error_report("WHPX: Failed to allocte VCPU context.");
        return -ENOMEM;
    }

#ifdef EMU_MICROSOFT
    hr = WHvEmulatorCreateEmulator(&whpx_emu_callbacks, &vcpu->emulator);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to setup instruction completion support,"
                     " hr=%08lx", hr);
        g_free(vcpu);
        return -EINVAL;
    }
#endif

    hr = WHvCreateVirtualProcessor(whpx->partition, cpu->cpu_index, 0);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to create a virtual processor part=%p idx=%d,"
            " hr=%08lx", whpx->partition, cpu->cpu_index, hr);
#ifdef EMU_MICROSOFT
        WHvEmulatorDestroyEmulator(vcpu->emulator);
#endif
        g_free(vcpu);
        return -EINVAL;
    }

    WHV_REGISTER_NAME name = WHvX64RegisterApicId;
    WHV_REGISTER_VALUE v = { .Reg64 = WHPX_LAPIC_ID(cpu->cpu_index) };
    hr = whpx_set_vp_registers(cpu->cpu_index,
        &name, 1, &v);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to set processor APIC ID,"
                      " hr=%08lx", hr);
#ifdef EMU_MICROSOFT
        WHvEmulatorDestroyEmulator(vcpu->emulator);
#endif
        WHvDeleteVirtualProcessor(whpx->partition, cpu->cpu_index);
        g_free(vcpu);
        return -EINVAL;
    }

    vcpu->dirty = VCPU_DIRTY_CPUSTATE;

    critical_section_init(&vcpu->irqreq_lock);

    cpu->hax_vcpu = (struct hax_vcpu_state *)vcpu;

    return 0;
}

int whpx_vcpu_exec(CPUState *cpu)
{
    int ret;
    int fatal;

    for (;;) {
        if (cpu->exception_index >= EXCP_INTERRUPT) {
            ret = cpu->exception_index;
            cpu->exception_index = -1;
            break;
        }

        fatal = whpx_vcpu_run(cpu);

        if (fatal) {
            error_report("WHPX: Failed to exec a virtual processor");
            abort();
        }
    }

    return ret;
}

void whpx_destroy_vcpu(CPUState *cpu)
{
    struct whpx_state *whpx = &whpx_global;
    struct whpx_vcpu *vcpu = whpx_vcpu(cpu);

    debug_printf("destroy vcpu %d\n", cpu->cpu_index);
    HRESULT hr = WHvDeleteVirtualProcessor(whpx->partition, cpu->cpu_index);
    if (FAILED(hr))
      whpx_panic("WHvDeleteVirtualProcessor[%d] failed: %x\n",
          cpu->cpu_index, (int)hr);
#ifdef EMU_MICROSOFT
    WHvEmulatorDestroyEmulator(vcpu->emulator);
#endif
    critical_section_free(&vcpu->irqreq_lock);
    g_free(cpu->hax_vcpu);
}

void
whpx_vcpu_kick(CPUState *cpu)
{
    struct whpx_state *whpx = &whpx_global;
    WHvCancelRunVirtualProcessor(whpx->partition, cpu->cpu_index, 0);
}

int
whpx_vcpu_get_context(CPUState *cpu, struct whpx_vcpu_context *ctx)
{
    struct whpx_vcpu *vcpu = whpx_vcpu(cpu);
    HRESULT hr;
    int i;
    uint32_t irq_bytes, xsave_bytes;
    uint8_t buf[PAGE_SIZE];

    assert(cpu_is_stopped(cpu));

    whpx_vcpu_flush_dirty(cpu);
    assert(!vcpu->dirty);

    ctx->interrupt_request = cpu->interrupt_request;
    ctx->interrupt_in_flight = vcpu->interrupt_in_flight;
    ctx->ready_for_pic_interrupt = vcpu->ready_for_pic_interrupt;
    ctx->window_registered = vcpu->window_registered;

    ctx->nreg = 0;
    whpx_reg_list_t *context_regs = whpx_all_registers();
    for (i = 0; i < context_regs->num; i++) {
        WHV_REGISTER_NAME n = context_regs->reg[i];
        WHV_REGISTER_VALUE *vp = (WHV_REGISTER_VALUE*)&ctx->regv[i];

        ctx->reg[i] = n;
        debug_printf("read register %s\n", get_whv_register_name_str(n));
        hr = whpx_get_vp_registers(cpu->cpu_index, &n, 1, (WHV_REGISTER_VALUE*)buf);
        if (FAILED(hr))
            whpx_panic("failed to access vcpu%d register %s\n",
                cpu->cpu_index, get_whv_register_name_str(n));
        memcpy(vp, buf, sizeof(*vp));
    }
    ctx->nreg = i;

    memset(ctx->irq_controller_state, 0, sizeof(ctx->irq_controller_state));
    hr = WHvGetVirtualProcessorInterruptControllerState(
        whpx_get_partition(),
        cpu->cpu_index,
        ctx->irq_controller_state,
        sizeof(ctx->irq_controller_state),
        &irq_bytes);
    if (FAILED(hr))
        whpx_panic("failed to get vcpu%d irq controller state: %08lx\n",
            cpu->cpu_index, hr);

    memset(ctx->xsave_state, 0, sizeof(ctx->xsave_state));
    hr = WHvGetVirtualProcessorXsaveState(
        whpx_get_partition(),
        cpu->cpu_index,
        ctx->xsave_state,
        sizeof(ctx->xsave_state),
        &xsave_bytes);
    if (FAILED(hr))
        whpx_panic("failed to get vcpu%d xsave state: %08lx\n",
            cpu->cpu_index, hr);

    debug_printf("irq state bytes %d, xsave state bytes %d\n",
      (int)irq_bytes, (int)xsave_bytes);

    whpx_dump_cpu_state(cpu->cpu_index);

    return 0;
}

int
whpx_vcpu_set_context(CPUState *cpu, struct whpx_vcpu_context *ctx)
{
    struct whpx_vcpu *vcpu = whpx_vcpu(cpu);
    HRESULT hr;
    int i;

    assert(cpu_is_stopped(cpu));

    cpu->interrupt_request = ctx->interrupt_request;
    vcpu->interrupt_in_flight = ctx->interrupt_in_flight;
    vcpu->window_registered = ctx->window_registered;
    vcpu->ready_for_pic_interrupt = ctx->ready_for_pic_interrupt;

    for (i = 0; i < ctx->nreg; i++) {
        WHV_REGISTER_NAME n = ctx->reg[i];
        WHV_REGISTER_VALUE vp;

        memcpy(&vp, &ctx->regv[i], sizeof(WHV_REGISTER_VALUE));

        hr = whpx_set_vp_registers(cpu->cpu_index, &n, 1, &vp);
        if (FAILED(hr))
            whpx_panic("failed to set vcpu%d register %s to value %"PRIx64"\n",
                cpu->cpu_index, get_whv_register_name_str(n), vp.Reg64);
    }

    hr = WHvSetVirtualProcessorInterruptControllerState(
        whpx_get_partition(),
        cpu->cpu_index,
        ctx->irq_controller_state,
        sizeof(ctx->irq_controller_state));
    if (FAILED(hr))
        whpx_panic("failed to set vcpu%d irq controller state: %08lx\n",
            cpu->cpu_index, hr);

    hr = WHvSetVirtualProcessorXsaveState(
        whpx_get_partition(),
        cpu->cpu_index,
        ctx->xsave_state,
        sizeof(ctx->xsave_state));
    if (FAILED(hr))
        whpx_panic("failed to set vcpu%d xsave state: %08lx\n",
            cpu->cpu_index, hr);

    vcpu->dirty = VCPU_DIRTY_HV;
    whpx_dump_cpu_state(cpu->cpu_index);

    return 0;
}

/*
 * Memory support.
 */

void
whpx_update_mapping(
    uint64_t start_pa, uint64_t size,
    void *host_va, int add, int rom,
    const char *name)
{
    struct whpx_state *whpx = &whpx_global;
    HRESULT hr;

#if 1
    if (add)
        debug_printf("WHPX: ADD PA:%016"PRIx64" Size:%"PRIx64", Host:%p, %s, '%s'\n",
            start_pa, size, host_va,
            (rom ? "ROM" : "RAM"), name);
    else
        debug_printf("WHPX: DEL PA:%016"PRIx64" Size:%"PRIx64", Host:%p,      '%s'\n",
            start_pa, size, host_va, name);
#endif

    if (add)
        hr = WHvMapGpaRange(whpx->partition,
                            host_va,
                            start_pa,
                            size,
                            (WHvMapGpaRangeFlagRead |
                             WHvMapGpaRangeFlagExecute |
                             (rom ? 0 : WHvMapGpaRangeFlagWrite)));
    else
        hr = WHvUnmapGpaRange(whpx->partition,
                              start_pa,
                              size);

    if (FAILED(hr))
        whpx_panic("Failed to %s GPA range '%s' PA:%016"PRIx64", Size:%"PRIx64
                   " bytes, Host:%p, hr=%08lx",
                   (add ? "MAP" : "UNMAP"), name, start_pa, size, host_va, hr);
}

void
whpx_cpu_reset_interrupt(CPUState *cpu, int mask)
{
    whpx_vcpu_irqreq_lock(cpu);
    cpu->interrupt_request &= ~mask;
    whpx_vcpu_irqreq_unlock(cpu);
}


static void
whpx_cpu_handle_interrupt(CPUState *cpu, int mask)
{
    whpx_vcpu_irqreq_lock(cpu);
    cpu->interrupt_request |= mask;
    whpx_vcpu_irqreq_unlock(cpu);
#ifdef DEBUG_IRQ
    debug_printf("vcpu%d: handle IRQ mask=%x irqreq=%x\n",
        cpu->cpu_index, mask, cpu->interrupt_request);
#endif

    if (!qemu_cpu_is_self(cpu))
        qemu_cpu_kick(cpu);
}

/*
 * Partition support
 */

int whpx_partition_init(void)
{
    struct whpx_state *whpx;
    int ret;
    HRESULT hr;
    WHV_CAPABILITY whpx_cap;
    WHV_CAPABILITY_FEATURES features = { };
    WHV_PARTITION_PROPERTY prop;
    whpx = &whpx_global;

    memset(whpx, 0, sizeof(struct whpx_state));
    whpx->mem_quota = vm_mem_mb << PAGE_SHIFT;

    hr = WHvGetCapability(WHvCapabilityCodeHypervisorPresent, &whpx_cap,
        sizeof(whpx_cap), NULL);
    if (FAILED(hr) || !whpx_cap.HypervisorPresent) {
        error_report("WHPX: No accelerator found, hr=%08lx", hr);
        ret = -ENOSPC;
        goto error;
    }

    hr = WHvGetCapability(WHvCapabilityCodeFeatures, &features,
        sizeof(features), NULL);
    if (FAILED(hr) || !features.LocalApicEmulation) {
        error_report("WHPX: No local apic emulation, hr=%08lx", hr);
        ret = -EINVAL;
        goto error;
    }

    hr = WHvCreatePartition(&whpx->partition);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to create partition, hr=%08lx", hr);
        ret = -EINVAL;
        goto error;
    }

    memset(&prop, 0, sizeof(WHV_PARTITION_PROPERTY));
    prop.LocalApicEmulationMode = WHvX64LocalApicEmulationModeXApic;
    hr = WHvSetPartitionProperty(whpx->partition,
        WHvPartitionPropertyCodeLocalApicEmulationMode,
        &prop,
        sizeof(prop));
    if (FAILED(hr)) {
        error_report("WHPX: Failed to enable local APIC hr=%08lx", hr);
        ret = -EINVAL;
        goto error;
    }

    memset(&prop, 0, sizeof(WHV_PARTITION_PROPERTY));
    prop.ProcessorCount = vm_vcpus;
    hr = WHvSetPartitionProperty(whpx->partition,
        WHvPartitionPropertyCodeProcessorCount,
        &prop, sizeof(WHV_PARTITION_PROPERTY));

    if (FAILED(hr)) {
        error_report("WHPX: Failed to set partition core count to %d,"
            " hr=%08lx", (int)vm_vcpus, hr);
        ret = -EINVAL;
        goto error;
    }

#ifdef XSAVE_DISABLED_UNTIL_FIXED
    memset(&prop, 0, sizeof(WHV_PARTITION_PROPERTY));
    prop.ProcessorXsaveFeatures.AsUINT64 = 0;
    hr = WHvSetPartitionProperty(whpx->partition,
        WHvPartitionPropertyCodeProcessorXsaveFeatures,
        &prop, sizeof(WHV_PARTITION_PROPERTY));

    if (FAILED(hr)) {
        error_report("WHPX: Failed to set partition core count to %d,"
            " hr=%08lx", (int)vm_vcpus, hr);
        ret = -EINVAL;
        goto error;
    }
#endif

    memset(&prop, 0, sizeof(WHV_PARTITION_PROPERTY));
    if (vm_viridian)
        prop.ExtendedVmExits.X64MsrExit = 1;
    prop.ExtendedVmExits.X64CpuidExit = 1;
    hr = WHvSetPartitionProperty(whpx->partition,
        WHvPartitionPropertyCodeExtendedVmExits,
        &prop, sizeof(WHV_PARTITION_PROPERTY));

    if (FAILED(hr)) {
        error_report("WHPX: Failed to set extended vm exits,"
            " hr=%08lx", hr);
        ret = -EINVAL;
        goto error;
    }

    hr = WHvSetupPartition(whpx->partition);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to setup partition, hr=%08lx", hr);
        ret = -EINVAL;
        goto error;
    }

    /* FIXME: this likely needs improvement */
    vm_id = WHPX_DOMAIN_ID_SELF;

    cpu_interrupt_handler = whpx_cpu_handle_interrupt;

    debug_printf("Windows Hypervisor Platform accelerator is operational\n");
    return 0;

  error:

    if (NULL != whpx->partition) {
        WHvDeletePartition(whpx->partition);
        whpx->partition = NULL;
    }


    return ret;
}

int
whpx_partition_destroy(void)
{
    HRESULT hr;
    struct whpx_state *whpx = &whpx_global;

    if (whpx->partition) {
        hr = WHvDeletePartition(whpx->partition);
        if (FAILED(hr))
            debug_printf("WHPX: Failed to delete partition, hr=%08lx", hr);
        whpx->partition = NULL;
    }

    return 0;
}

void
whpx_set_dm_features(uint64_t features)
{
    whpx_global.dm_features = features;
}

void
whpx_set_random_seed(uint64_t lo, uint64_t hi)
{
    whpx_global.seed_lo = lo;
    whpx_global.seed_hi = hi;
}
