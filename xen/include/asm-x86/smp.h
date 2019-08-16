#ifndef __ASM_SMP_H
#define __ASM_SMP_H

/*
 * We need the APIC definitions automatically as part of 'smp.h'
 */
#ifndef __ASSEMBLY__
#include <xen/config.h>
#include <xen/kernel.h>
#include <xen/cpumask.h>
#include <asm/current.h>
#endif

#ifndef __ASSEMBLY__
#include <asm/bitops.h>
#include <asm/mpspec.h>
#endif

#define BAD_APICID -1U
#ifdef CONFIG_SMP
#ifndef __ASSEMBLY__

/*
 * Private routines/data
 */
 
extern void smp_alloc_memory(void);
DECLARE_PER_CPU(cpumask_var_t, cpu_sibling_mask);
DECLARE_PER_CPU(cpumask_var_t, cpu_core_mask);

void smp_send_nmi_allbutself(void);

void uxen_ipi_mask(const cpumask_t *, int);
#define send_IPI_mask(m, v) uxen_ipi_mask(m, v)

extern void (*mtrr_hook) (void);

#define MAX_APICID 256
extern u32 x86_cpu_to_apicid[];
extern u32 cpu_2_logical_apicid[];

#define cpu_physical_id(cpu)	x86_cpu_to_apicid[cpu]

#define cpu_is_offline(cpu) unlikely(!cpu_online(cpu))
extern void cpu_exit_clear(unsigned int cpu);
extern void cpu_uninit(unsigned int cpu);
int cpu_add(uint32_t apic_id, uint32_t acpi_id, uint32_t pxm);

/*
 * This function is needed by all SMP systems. It must _always_ be valid
 * from the initial startup. We map APIC_BASE very early in page_setup(),
 * so this is correct in the x86 case.
 */
#define raw_smp_processor_id() (get_processor_id())

int hard_smp_processor_id(void);
int logical_smp_processor_id(void);

void __stop_this_cpu(void);

#endif /* !__ASSEMBLY__ */

#else /* CONFIG_SMP */

#define cpu_physical_id(cpu)		boot_cpu_physical_apicid

#define NO_PROC_ID		0xFF		/* No processor magic marker */

#endif
#endif
