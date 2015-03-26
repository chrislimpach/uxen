/*
 * Copyright 2014-2015, Bromium, Inc.
 * Author: Julian Pidancet <julian@pidancet.net>
 * SPDX-License-Identifier: ISC
 */

#ifndef _HYPERCALL_H_
#define _HYPERCALL_H_

static inline void
cpuid(uint32_t idx,
      uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
{
    asm volatile ("cpuid"
                  : "=a" (*eax),
                    "=b" (*ebx),
                    "=c" (*ecx),
                    "=d" (*edx)
                  : "0" (idx)
                  : "memory");
}

static inline void
wrmsr(uint32_t reg, uint64_t val)
{
    asm volatile ("wrmsr"
                  :
                  : "c" (reg), "a" ((uint32_t)val),
                    "d" ((uint32_t)(val >> 32)));
}

/* osfmk/kern/bsd_kern.c */
static inline vm_map_t
get_task_map(task_t t)
{
    return *(vm_map_t *)((uint8_t *)t + 0x20);
}

#define hcall(name) \
    (((uintptr_t)(hypercall_desc->getBytesNoCopy())) + (__HYPERVISOR_##name * 32))
#define hcall_arg(x) ((uintptr_t)(x))

#ifndef ENOENT
#define ENOENT 2
#endif

extern "C" uintptr_t _hypercall0(uintptr_t addr);
extern "C" uintptr_t _hypercall1(uintptr_t addr, uintptr_t arg1);
extern "C" uintptr_t _hypercall2(uintptr_t addr, uintptr_t arg1, uintptr_t arg2);
extern "C" uintptr_t _hypercall3(uintptr_t addr, uintptr_t arg1, uintptr_t arg2,
                      uintptr_t arg3);
extern "C" uintptr_t _hypercall4(uintptr_t addr, uintptr_t arg1, uintptr_t arg2,
                      uintptr_t arg3, uintptr_t arg4);
extern "C" uintptr_t _hypercall5(uintptr_t addr, uintptr_t arg1, uintptr_t arg2,
                      uintptr_t arg3, uintptr_t arg4, uintptr_t arg5);

#endif /* _HYPERCALL_H_ */
