/*
 *  High Precisition Event Timer emulation
 *
 *  Copyright (c) 2007 Alexander Graf
 *  Copyright (c) 2008 IBM Corporation
 *
 *  Authors: Beth Kon <bkon@us.ibm.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * *****************************************************************
 *
 * This driver attempts to emulate an HPET device in software.
 */
/*
 * uXen changes:
 *
 * Copyright 2018-2019, Bromium, Inc.
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
#include <dm/qemu/hw/sysbus.h>
#include <dm/qemu/host-utils.h>
#include <dm/dm.h>
#include <dm/timer.h>
#include <dm/debug.h>
#include <dm/whpx/hpet_emul.h>
#include <time.h>
#include <dm/whpx/mc146818rtc.h>
#include <dm/whpx/util.h>

//#define HPET_DEBUG
#ifdef HPET_DEBUG
#define DPRINTF printf
#else
#define DPRINTF(...)
#endif

#define HPET_MSI_SUPPORT        0

struct HPETState;
typedef struct HPETTimer {  /* timers */
    uint8_t tn;             /*timer number*/
    QEMUTimer *qemu_timer;
    struct HPETState *state;
    /* Memory-mapped, software visible timer registers */
    uint64_t config;        /* configuration/cap */
    uint64_t cmp;           /* comparator */
    uint64_t fsb;           /* FSB route */
    /* Hidden register state */
    uint64_t period;        /* Last value written to comparator */
    uint8_t wrap_flag;      /* timer pop will indicate wrap for one-shot 32-bit
                             * mode. Next pop will be actual timer expiration.
                             */
} HPETTimer;

typedef struct HPETState {
    SysBusDevice busdev;
    MemoryRegion io_memory;
    uint64_t hpet_offset;
    qemu_irq irqs[HPET_NUM_IRQ_ROUTES];
    uint32_t flags;
    uint8_t rtc_irq_level;
    uint8_t num_timers;
    HPETTimer timer[HPET_MAX_TIMERS];

    /* Memory-mapped, software visible registers */
    uint64_t capability;        /* capabilities */
    uint64_t config;            /* configuration */
    uint64_t isr;               /* interrupt status reg */
    uint64_t hpet_counter;      /* main counter */
    uint8_t  hpet_id;           /* instance id */
} HPETState;

void hpet_pit_disable(void);
void hpet_pit_enable(void);

struct hpet_fw_config hpet_cfg = {.count = UINT8_MAX};

static uint32_t hpet_in_legacy_mode(HPETState *s)
{
    return s->config & HPET_CFG_LEGACY;
}

static uint32_t timer_int_route(struct HPETTimer *timer)
{
    return (timer->config & HPET_TN_INT_ROUTE_MASK) >> HPET_TN_INT_ROUTE_SHIFT;
}

static uint32_t timer_fsb_route(HPETTimer *t)
{
    return t->config & HPET_TN_FSB_ENABLE;
}

static uint32_t hpet_enabled(HPETState *s)
{
    return s->config & HPET_CFG_ENABLE;
}

static uint32_t timer_is_periodic(HPETTimer *t)
{
    return t->config & HPET_TN_PERIODIC;
}

static uint32_t timer_enabled(HPETTimer *t)
{
    return t->config & HPET_TN_ENABLE;
}

static uint32_t hpet_time_after(uint64_t a, uint64_t b)
{
    return ((int32_t)(b) - (int32_t)(a) < 0);
}

static uint32_t hpet_time_after64(uint64_t a, uint64_t b)
{
    return ((int64_t)(b) - (int64_t)(a) < 0);
}

static uint64_t ticks_to_ns(uint64_t value)
{
    return (muldiv64(value, HPET_CLK_PERIOD, FS_PER_NS));
}

static uint64_t ns_to_ticks(uint64_t value)
{
    return (muldiv64(value, FS_PER_NS, HPET_CLK_PERIOD));
}

static uint64_t hpet_fixup_reg(uint64_t new, uint64_t old, uint64_t mask)
{
    new &= mask;
    new |= old & ~mask;
    return new;
}

static int activating_bit(uint64_t old, uint64_t new, uint64_t mask)
{
    return (!(old & mask) && (new & mask));
}

static int deactivating_bit(uint64_t old, uint64_t new, uint64_t mask)
{
    return ((old & mask) && !(new & mask));
}

static uint64_t hpet_get_ticks(HPETState *s)
{
    return ns_to_ticks(qemu_get_clock_ns(vm_clock) + s->hpet_offset);
}

/*
 * calculate diff between comparator value and current ticks
 */
static inline uint64_t hpet_calculate_diff(HPETTimer *t, uint64_t current)
{

    if (t->config & HPET_TN_32BIT) {
        uint32_t diff, cmp;

        cmp = (uint32_t)t->cmp;
        diff = cmp - (uint32_t)current;
        diff = (int32_t)diff > 0 ? diff : (uint32_t)1;
        return (uint64_t)diff;
    } else {
        uint64_t diff, cmp;

        cmp = t->cmp;
        diff = cmp - current;
        diff = (int64_t)diff > 0 ? diff : (uint64_t)1;
        return diff;
    }
}

static void update_irq(struct HPETTimer *timer, int set)
{
    uint64_t mask;
    HPETState *s;
    int route;

    if (timer->tn <= 1 && hpet_in_legacy_mode(timer->state)) {
        /* if LegacyReplacementRoute bit is set, HPET specification requires
         * timer0 be routed to IRQ0 in NON-APIC or IRQ2 in the I/O APIC,
         * timer1 be routed to IRQ8 in NON-APIC or IRQ8 in the I/O APIC.
         */
        route = (timer->tn == 0) ? 0 : RTC_ISA_IRQ;
    } else {
        route = timer_int_route(timer);
    }
    s = timer->state;
    mask = 1 << timer->tn;
    if (!set || !timer_enabled(timer) || !hpet_enabled(timer->state)) {
        s->isr &= ~mask;
        if (!timer_fsb_route(timer)) {
            qemu_irq_lower(s->irqs[route]);
        }
    } else if (timer_fsb_route(timer)) {
#if 0
        stl_le_phys(timer->fsb >> 32, timer->fsb & 0xffffffff);
#endif
        uint32_t fsb_lo = timer->fsb & 0xffffffff;
        vm_memory_rw(timer->fsb >> 32, (uint8_t*)&fsb_lo, sizeof(fsb_lo), 1);
    } else if (timer->config & HPET_TN_TYPE_LEVEL) {
        s->isr |= mask;
        qemu_irq_raise(s->irqs[route]);
    } else {
        s->isr &= ~mask;
        qemu_irq_pulse(s->irqs[route]);
    }
}

static void hpet_pre_save(void *opaque)
{
    HPETState *s = opaque;

    /* save current counter value */
    s->hpet_counter = hpet_get_ticks(s);
}

static int hpet_pre_load(void *opaque)
{
    HPETState *s = opaque;

    /* version 1 only supports 3, later versions will load the actual value */
    s->num_timers = HPET_MIN_TIMERS;
    return 0;
}

static int hpet_post_load(void *opaque, int version_id)
{
    HPETState *s = opaque;

    /* Recalculate the offset between the main counter and guest time */
    s->hpet_offset = ticks_to_ns(s->hpet_counter) - qemu_get_clock_ns(vm_clock);

    /* Push number of timers into capability returned via HPET_ID */
    s->capability &= ~HPET_ID_NUM_TIM_MASK;
    s->capability |= (s->num_timers - 1) << HPET_ID_NUM_TIM_SHIFT;
    hpet_cfg.hpet[s->hpet_id].event_timer_block_id = (uint32_t)s->capability;

    /* Derive HPET_MSI_SUPPORT from the capability of the first timer. */
    s->flags &= ~(1 << HPET_MSI_SUPPORT);
    if (s->timer[0].config & HPET_TN_FSB_CAP) {
        s->flags |= 1 << HPET_MSI_SUPPORT;
    }
    return 0;
}

static const VMStateDescription vmstate_hpet_timer = {
    .name = "hpet_timer",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField []) {
        VMSTATE_UINT8(tn, HPETTimer),
        VMSTATE_UINT64(config, HPETTimer),
        VMSTATE_UINT64(cmp, HPETTimer),
        VMSTATE_UINT64(fsb, HPETTimer),
        VMSTATE_UINT64(period, HPETTimer),
        VMSTATE_UINT8(wrap_flag, HPETTimer),
        VMSTATE_TIMER(qemu_timer, HPETTimer),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_hpet = {
    .name = "hpet",
    .version_id = 2,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .pre_save = hpet_pre_save,
    .pre_load = hpet_pre_load,
    .post_load = hpet_post_load,
    .fields      = (VMStateField []) {
        VMSTATE_UINT64(config, HPETState),
        VMSTATE_UINT64(isr, HPETState),
        VMSTATE_UINT64(hpet_counter, HPETState),
        VMSTATE_UINT8_V(num_timers, HPETState, 2),
        VMSTATE_STRUCT_VARRAY_UINT8(timer, HPETState, num_timers, 0,
                                    vmstate_hpet_timer, HPETTimer),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * timer expiration callback
 */
static void hpet_timer(void *opaque)
{
    HPETTimer *t = opaque;
    uint64_t diff;

    uint64_t period = t->period;
    uint64_t cur_tick = hpet_get_ticks(t->state);

    if (timer_is_periodic(t) && period != 0) {
        if (t->config & HPET_TN_32BIT) {
            while (hpet_time_after(cur_tick, t->cmp)) {
                t->cmp = (uint32_t)(t->cmp + t->period);
            }
        } else {
            while (hpet_time_after64(cur_tick, t->cmp)) {
                t->cmp += period;
            }
        }
        diff = hpet_calculate_diff(t, cur_tick);
        qemu_mod_timer(t->qemu_timer,
                       qemu_get_clock_ns(vm_clock) + (int64_t)ticks_to_ns(diff));
    } else if (t->config & HPET_TN_32BIT && !timer_is_periodic(t)) {
        if (t->wrap_flag) {
            diff = hpet_calculate_diff(t, cur_tick);
            qemu_mod_timer(t->qemu_timer, qemu_get_clock_ns(vm_clock) +
                           (int64_t)ticks_to_ns(diff));
            t->wrap_flag = 0;
        }
    }
    update_irq(t, 1);
}

static void hpet_set_timer(HPETTimer *t)
{
    uint64_t diff;
    uint32_t wrap_diff;  /* how many ticks until we wrap? */
    uint64_t cur_tick = hpet_get_ticks(t->state);

    /* whenever new timer is being set up, make sure wrap_flag is 0 */
    t->wrap_flag = 0;
    diff = hpet_calculate_diff(t, cur_tick);

    /* hpet spec says in one-shot 32-bit mode, generate an interrupt when
     * counter wraps in addition to an interrupt with comparator match.
     */
    if (t->config & HPET_TN_32BIT && !timer_is_periodic(t)) {
        wrap_diff = 0xffffffff - (uint32_t)cur_tick;
        if (wrap_diff < (uint32_t)diff) {
            diff = wrap_diff;
            t->wrap_flag = 1;
        }
    }
    qemu_mod_timer(t->qemu_timer,
                   qemu_get_clock_ns(vm_clock) + (int64_t)ticks_to_ns(diff));
}

static void hpet_del_timer(HPETTimer *t)
{
    qemu_del_timer(t->qemu_timer);
    update_irq(t, 0);
}

#ifdef HPET_DEBUG
static uint32_t hpet_ram_readb(void *opaque, target_phys_addr_t addr)
{
    printf("qemu: hpet_read b at %" PRIx64 "\n", addr);
    return 0;
}

static uint32_t hpet_ram_readw(void *opaque, target_phys_addr_t addr)
{
    printf("qemu: hpet_read w at %" PRIx64 "\n", addr);
    return 0;
}
#endif

static uint32_t hpet_ram_readl(void *opaque, target_phys_addr_t addr)
{
    HPETState *s = opaque;
    uint64_t cur_tick, index;

    DPRINTF("qemu: Enter hpet_ram_readl at %" PRIx64 "\n", addr);
    index = addr;
    /*address range of all TN regs*/
    if (index >= 0x100 && index <= 0x3ff) {
        uint8_t timer_id = (addr - 0x100) / 0x20;
        HPETTimer *timer = &s->timer[timer_id];

        if (timer_id > s->num_timers) {
            DPRINTF("qemu: timer id out of range\n");
            return 0;
        }

        switch ((addr - 0x100) % 0x20) {
        case HPET_TN_CFG:
            return timer->config;
        case HPET_TN_CFG + 4: // Interrupt capabilities
            return timer->config >> 32;
        case HPET_TN_CMP: // comparator register
            return timer->cmp;
        case HPET_TN_CMP + 4:
            return timer->cmp >> 32;
        case HPET_TN_ROUTE:
            return timer->fsb;
        case HPET_TN_ROUTE + 4:
            return timer->fsb >> 32;
        default:
            DPRINTF("qemu: invalid hpet_ram_readl\n");
            break;
        }
    } else {
        switch (index) {
        case HPET_ID:
            return s->capability;
        case HPET_PERIOD:
            return s->capability >> 32;
        case HPET_CFG:
            return s->config;
        case HPET_CFG + 4:
            DPRINTF("qemu: invalid HPET_CFG + 4 hpet_ram_readl\n");
            return 0;
        case HPET_COUNTER:
            if (hpet_enabled(s)) {
                cur_tick = hpet_get_ticks(s);
            } else {
                cur_tick = s->hpet_counter;
            }
            DPRINTF("qemu: reading counter  = %" PRIx64 "\n", cur_tick);
            return cur_tick;
        case HPET_COUNTER + 4:
            if (hpet_enabled(s)) {
                cur_tick = hpet_get_ticks(s);
            } else {
                cur_tick = s->hpet_counter;
            }
            DPRINTF("qemu: reading counter + 4  = %" PRIx64 "\n", cur_tick);
            return cur_tick >> 32;
        case HPET_STATUS:
            return s->isr;
        default:
            DPRINTF("qemu: invalid hpet_ram_readl\n");
            break;
        }
    }
    return 0;
}

#ifdef HPET_DEBUG
static void hpet_ram_writeb(void *opaque, target_phys_addr_t addr,
                            uint32_t value)
{
    printf("qemu: invalid hpet_write b at %" PRIx64 " = %#x\n",
           addr, value);
}

static void hpet_ram_writew(void *opaque, target_phys_addr_t addr,
                            uint32_t value)
{
    printf("qemu: invalid hpet_write w at %" PRIx64 " = %#x\n",
           addr, value);
}
#endif

static void hpet_ram_writel(void *opaque, target_phys_addr_t addr,
                            uint32_t value)
{
    int i;
    HPETState *s = opaque;
    uint64_t old_val, new_val, val, index;

    DPRINTF("qemu: Enter hpet_ram_writel at %" PRIx64 " = %#x\n", addr, value);
    index = addr;
    old_val = hpet_ram_readl(opaque, addr);
    new_val = value;

    /*address range of all TN regs*/
    if (index >= 0x100 && index <= 0x3ff) {
        uint8_t timer_id = (addr - 0x100) / 0x20;
        HPETTimer *timer = &s->timer[timer_id];

        DPRINTF("qemu: hpet_ram_writel timer_id = %#x\n", timer_id);
        if (timer_id > s->num_timers) {
            DPRINTF("qemu: timer id out of range\n");
            return;
        }
        switch ((addr - 0x100) % 0x20) {
        case HPET_TN_CFG:
            DPRINTF("qemu: hpet_ram_writel HPET_TN_CFG\n");
            if (activating_bit(old_val, new_val, HPET_TN_FSB_ENABLE)) {
                update_irq(timer, 0);
            }
            val = hpet_fixup_reg(new_val, old_val, HPET_TN_CFG_WRITE_MASK);
            timer->config = (timer->config & 0xffffffff00000000ULL) | val;
            if (new_val & HPET_TN_32BIT) {
                timer->cmp = (uint32_t)timer->cmp;
                timer->period = (uint32_t)timer->period;
            }
            if (activating_bit(old_val, new_val, HPET_TN_ENABLE)) {
                hpet_set_timer(timer);
            } else if (deactivating_bit(old_val, new_val, HPET_TN_ENABLE)) {
                hpet_del_timer(timer);
            }
            break;
        case HPET_TN_CFG + 4: // Interrupt capabilities
            DPRINTF("qemu: invalid HPET_TN_CFG+4 write\n");
            break;
        case HPET_TN_CMP: // comparator register
            DPRINTF("qemu: hpet_ram_writel HPET_TN_CMP\n");
            if (timer->config & HPET_TN_32BIT) {
                new_val = (uint32_t)new_val;
            }
            if (!timer_is_periodic(timer)
                || (timer->config & HPET_TN_SETVAL)) {
                timer->cmp = (timer->cmp & 0xffffffff00000000ULL) | new_val;
            }
            if (timer_is_periodic(timer)) {
                /*
                 * FIXME: Clamp period to reasonable min value?
                 * Clamp period to reasonable max value
                 */
                new_val &= (timer->config & HPET_TN_32BIT ? ~0u : ~0ull) >> 1;
                timer->period =
                    (timer->period & 0xffffffff00000000ULL) | new_val;
            }
            timer->config &= ~HPET_TN_SETVAL;
            if (hpet_enabled(s)) {
                hpet_set_timer(timer);
            }
            break;
        case HPET_TN_CMP + 4: // comparator register high order
            DPRINTF("qemu: hpet_ram_writel HPET_TN_CMP + 4\n");
            if (!timer_is_periodic(timer)
                || (timer->config & HPET_TN_SETVAL)) {
                timer->cmp = (timer->cmp & 0xffffffffULL) | new_val << 32;
            } else {
                /*
                 * FIXME: Clamp period to reasonable min value?
                 * Clamp period to reasonable max value
                 */
                new_val &= (timer->config & HPET_TN_32BIT ? ~0u : ~0ull) >> 1;
                timer->period =
                    (timer->period & 0xffffffffULL) | new_val << 32;
                }
                timer->config &= ~HPET_TN_SETVAL;
                if (hpet_enabled(s)) {
                    hpet_set_timer(timer);
                }
                break;
        case HPET_TN_ROUTE:
            timer->fsb = (timer->fsb & 0xffffffff00000000ULL) | new_val;
            break;
        case HPET_TN_ROUTE + 4:
            timer->fsb = (new_val << 32) | (timer->fsb & 0xffffffff);
            break;
        default:
            DPRINTF("qemu: invalid hpet_ram_writel\n");
            break;
        }
        return;
    } else {
        switch (index) {
        case HPET_ID:
            return;
        case HPET_CFG:
            val = hpet_fixup_reg(new_val, old_val, HPET_CFG_WRITE_MASK);
            s->config = (s->config & 0xffffffff00000000ULL) | val;
            if (activating_bit(old_val, new_val, HPET_CFG_ENABLE)) {
                /* Enable main counter and interrupt generation. */
                s->hpet_offset =
                    ticks_to_ns(s->hpet_counter) - qemu_get_clock_ns(vm_clock);
                for (i = 0; i < s->num_timers; i++) {
                    if ((&s->timer[i])->cmp != ~0ULL) {
                        hpet_set_timer(&s->timer[i]);
                    }
                }
            } else if (deactivating_bit(old_val, new_val, HPET_CFG_ENABLE)) {
                /* Halt main counter and disable interrupt generation. */
                s->hpet_counter = hpet_get_ticks(s);
                for (i = 0; i < s->num_timers; i++) {
                    hpet_del_timer(&s->timer[i]);
                }
            }
            /* i8254 and RTC are disabled when HPET is in legacy mode */
            if (activating_bit(old_val, new_val, HPET_CFG_LEGACY)) {
                hpet_pit_disable();
                qemu_irq_lower(s->irqs[RTC_ISA_IRQ]);
            } else if (deactivating_bit(old_val, new_val, HPET_CFG_LEGACY)) {
                hpet_pit_enable();
                qemu_set_irq(s->irqs[RTC_ISA_IRQ], s->rtc_irq_level);
            }
            break;
        case HPET_CFG + 4:
            DPRINTF("qemu: invalid HPET_CFG+4 write\n");
            break;
        case HPET_STATUS:
            val = new_val & s->isr;
            for (i = 0; i < s->num_timers; i++) {
                if (val & (1 << i)) {
                    update_irq(&s->timer[i], 0);
                }
            }
            break;
        case HPET_COUNTER:
            if (hpet_enabled(s)) {
                DPRINTF("qemu: Writing counter while HPET enabled!\n");
            }
            s->hpet_counter =
                (s->hpet_counter & 0xffffffff00000000ULL) | value;
            DPRINTF("qemu: HPET counter written. ctr = %#x -> %" PRIx64 "\n",
                    value, s->hpet_counter);
            break;
        case HPET_COUNTER + 4:
            if (hpet_enabled(s)) {
                DPRINTF("qemu: Writing counter while HPET enabled!\n");
            }
            s->hpet_counter =
                (s->hpet_counter & 0xffffffffULL) | (((uint64_t)value) << 32);
            DPRINTF("qemu: HPET counter + 4 written. ctr = %#x -> %" PRIx64 "\n",
                    value, s->hpet_counter);
            break;
        default:
            DPRINTF("qemu: invalid hpet_ram_writel\n");
            break;
        }
    }
}

static CPUReadMemoryFunc * const hpet_ram_read[] = {
#ifdef HPET_DEBUG
    hpet_ram_readb,
    hpet_ram_readw,
#else
    NULL,
    NULL,
#endif
    hpet_ram_readl,
};

static CPUWriteMemoryFunc * const hpet_ram_write[] = {
#ifdef HPET_DEBUG
    hpet_ram_writeb,
    hpet_ram_writew,
#else
    NULL,
    NULL,
#endif
    hpet_ram_writel,
};

static uint64_t hpet_mem_read(void *opaque, target_phys_addr_t addr,
                                unsigned size)
{
  count_hpet++;
  switch (size) {
      //case 1: return hpet_ram_readb(opaque, addr);
      //case 2: return hpet_ram_readw(opaque, addr);
  case 4: return hpet_ram_readl(opaque, addr);
  default: assert(0); return 0;
  }
}

static void hpet_mem_write(void *opaque, target_phys_addr_t addr,
                             uint64_t val, unsigned size)
{
  count_hpet++;
  switch (size) {
      //case 1: hpet_ram_writeb(opaque, addr, val); break;
      //case 2: hpet_ram_writew(opaque, addr, val); break;
  case 4: hpet_ram_writel(opaque, addr, val); break;
  default: assert(0); break;
  }
}

static void hpet_reset(DeviceState *d)
{
    HPETState *s = FROM_SYSBUS(HPETState, sysbus_from_qdev(d));
    int i;
    static int count = 0;

    for (i = 0; i < s->num_timers; i++) {
        HPETTimer *timer = &s->timer[i];

        hpet_del_timer(timer);
        timer->cmp = ~0ULL;
        timer->config = HPET_TN_PERIODIC_CAP | HPET_TN_SIZE_CAP;
        if (s->flags & (1 << HPET_MSI_SUPPORT)) {
            timer->config |= HPET_TN_FSB_CAP;
        }
        /* advertise availability of ioapic inti2 */
        timer->config |=  0x00000004ULL << 32;
        timer->period = 0ULL;
        timer->wrap_flag = 0;
    }

    s->hpet_counter = 0ULL;
    s->hpet_offset = 0ULL;
    s->config = 0ULL;
    if (count > 0) {
        /* we don't enable pit when hpet_reset is first called (by hpet_init)
         * because hpet is taking over for pit here. On subsequent invocations,
         * hpet_reset is called due to system reset. At this point control must
         * be returned to pit until SW reenables hpet.
         */
        hpet_pit_enable();
    }
    hpet_cfg.hpet[s->hpet_id].event_timer_block_id = (uint32_t)s->capability;
    hpet_cfg.hpet[s->hpet_id].address = sysbus_from_qdev(d)->mmio[0].addr;
    count = 1;
}

static void hpet_handle_rtc_irq(void *opaque, int n, int level)
{
    HPETState *s = opaque;

    s->rtc_irq_level = level;
    if (!hpet_in_legacy_mode(s)) {
        qemu_set_irq(s->irqs[RTC_ISA_IRQ], level);
    }
}

static void
mmio_ptr_update(void *ptr, void *opaque)
{
}

static const MemoryRegionOps hpet_io_ops = {
    .read = hpet_mem_read,
    .write = hpet_mem_write
};

static int hpet_init1(SysBusDevice *dev)
{
    HPETState *s = FROM_SYSBUS(HPETState, dev);
    int i;
    HPETTimer *timer;

    if (hpet_cfg.count == UINT8_MAX) {
        /* first instance */
        hpet_cfg.count = 0;
    }

    if (hpet_cfg.count == 8) {
        fprintf(stderr, "Only 8 instances of HPET is allowed\n");
        return -1;
    }

    s->hpet_id = hpet_cfg.count++;

    if (s->num_timers < HPET_MIN_TIMERS) {
        s->num_timers = HPET_MIN_TIMERS;
    } else if (s->num_timers > HPET_MAX_TIMERS) {
        s->num_timers = HPET_MAX_TIMERS;
    }
    for (i = 0; i < HPET_MAX_TIMERS; i++) {
        timer = &s->timer[i];
        timer->qemu_timer = qemu_new_timer_ns(vm_clock, hpet_timer, timer);
        timer->tn = i;
        timer->state = s;
    }

    /* 64-bit main counter; LegacyReplacementRoute. */
    s->capability = 0x8086a001ULL;
    s->capability |= (s->num_timers - 1) << HPET_ID_NUM_TIM_SHIFT;
    s->capability |= ((HPET_CLK_PERIOD) << 32);

    memory_region_init_io(&s->io_memory, &hpet_io_ops, s, "hpet", 0x400);
    memory_region_add_ram_range(&s->io_memory, 0, 0x400,
                                mmio_ptr_update, s);
    memory_region_add_subregion(system_iomem, 0xfed00000, &s->io_memory);

    return 0;
}

int
hpet_init(qemu_irq *gsis, qemu_irq *out_rtc_irq)
{
    DeviceState *dev;
    HPETState *s;
    qemu_irq *irq_arr;
    int i;

    dev = qdev_create(NULL, "hpet");
    assert(dev);

    s = DO_UPCAST(HPETState, busdev.qdev, dev);
    /* connect hpet to GSI irqs */
    for (i = 0; i < HPET_NUM_IRQ_ROUTES; i++)
        s->irqs[i] = gsis[i];
    /* provide rtc irq handle for external party (rtc) to set */
    irq_arr = qemu_allocate_irqs(hpet_handle_rtc_irq, s, 1);
    *out_rtc_irq = irq_arr[0];
    
    qdev_init_nofail(dev);

    return 0;
}

static SysBusDeviceInfo hpet_device_info = {
    .qdev.name    = "hpet",
    .qdev.size    = sizeof(HPETState),
#if 0
    .qdev.no_user = 1,
#endif
    .qdev.vmsd    = &vmstate_hpet,
    .qdev.reset   = hpet_reset,
    .init         = hpet_init1,
    .qdev.props = (Property[]) {
        DEFINE_PROP_UINT8("timers", HPETState, num_timers, HPET_MIN_TIMERS),
        DEFINE_PROP_BIT("msi", HPETState, flags, HPET_MSI_SUPPORT, false),
        DEFINE_PROP_END_OF_LIST(),
    },
};

static void hpet_register_device(void)
{
    sysbus_register_withprop(&hpet_device_info);
}

device_init(hpet_register_device)
