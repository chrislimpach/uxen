/******************************************************************************
 * arch/x86/mm/p2m.c
 *
 * physical-to-machine mappings for automatically-translated domains.
 *
 * Parts of this code are Copyright (c) 2009 by Citrix Systems, Inc. (Patrick Colp)
 * Parts of this code are Copyright (c) 2007 by Advanced Micro Devices.
 * Parts of this code are Copyright (c) 2006-2007 by XenSource Inc.
 * Parts of this code are Copyright (c) 2006 by Michael A Fetterman
 * Parts based on earlier work by Michael A Fetterman, Ian Pratt et al.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
/*
 * uXen changes:
 *
 * Copyright 2011-2016, Bromium, Inc.
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

#include <asm/domain.h>
#include <asm/page.h>
#include <asm/paging.h>
#include <asm/p2m.h>
#include <asm/hvm/vmx/vmx.h> /* ept_p2m_init() */
#ifndef __UXEN__
#include <xen/iommu.h>
#endif  /* __UXEN__ */
#include <asm/mem_event.h>
#include <public/mem_event.h>
#include <asm/mem_sharing.h>
#include <xen/event.h>
#ifndef __UXEN__
#include <asm/hvm/nestedhvm.h>
#include <asm/hvm/svm/amd-iommu-proto.h>
#endif  /* __UXEN__ */
#include <uxen/memcache-dm.h>

#include "mm-locks.h"

int p2m_debug_more = 0;

/* turn on/off 1GB host page table support for hap, default on */
static bool_t __read_mostly opt_hap_1gb = 1;
boolean_param("hap_1gb", opt_hap_1gb);

static bool_t __read_mostly opt_hap_2mb = 1;
boolean_param("hap_2mb", opt_hap_2mb);

/* Printouts */
#define P2M_PRINTK(_f, _a...)                                \
    debugtrace_printk("p2m: %s(): " _f, __func__, ##_a)
#define P2M_ERROR(_f, _a...)                                 \
    printk("pg error: %s(): " _f, __func__, ##_a)
#if P2M_DEBUGGING
#define P2M_DEBUG(_f, _a...)                                 \
    debugtrace_printk("p2mdebug: %s(): " _f, __func__, ##_a)
#else
#define P2M_DEBUG(_f, _a...) do { (void)(_f); } while(0)
#endif


/* Override macros from asm/page.h to make them work with mfn_t */
#undef mfn_to_page
#define mfn_to_page(_m) __mfn_to_page(mfn_x(_m))
#undef mfn_valid
#define mfn_valid(_mfn) __mfn_valid(mfn_x(_mfn))
#undef mfn_valid_page
#define mfn_valid_page(_mfn) __mfn_valid_page(mfn_x(_mfn))
#undef page_to_mfn
#define page_to_mfn(_pg) _mfn(__page_to_mfn(_pg))


static void p2m_l1_cache_flush(void);


/* Init the datastructures for later use by the p2m code */
static void p2m_initialise(struct domain *d, struct p2m_domain *p2m)
{
    mm_lock_init(&p2m->lock);
    mm_lock_init(&p2m->logdirty_lock);
#ifndef __UXEN__
    INIT_LIST_HEAD(&p2m->np2m_list);
#endif  /* __UXEN__ */
    INIT_PAGE_LIST_HEAD(&p2m->pages);
    INIT_PAGE_LIST_HEAD(&p2m->pod.super);
    INIT_PAGE_LIST_HEAD(&p2m->pod.single);

    p2m->domain = d;
    p2m->default_access = p2m_access_rwx;

#ifndef __UXEN__
    p2m->cr3 = CR3_EADDR;
#endif  /* __UXEN__ */

    printk("vm%u: hap %sabled boot_cpu_data.x86_vendor %s\n",
           d->domain_id, hap_enabled(d) ? "en" : "dis",
           (boot_cpu_data.x86_vendor ==  X86_VENDOR_INTEL) ? "intel" :
           ((boot_cpu_data.x86_vendor ==  X86_VENDOR_AMD) ? "amd" :
            "unsupported"));
    if ( hap_enabled(d) && (boot_cpu_data.x86_vendor == X86_VENDOR_INTEL) )
        ept_p2m_init(p2m);
    else if ( hap_enabled(d) && (boot_cpu_data.x86_vendor == X86_VENDOR_AMD) )
        p2m_pt_init(p2m);
    else
        if (d->domain_id && d->domain_id < DOMID_FIRST_RESERVED) DEBUG();

    return;
}

#ifndef __UXEN__
static int
p2m_init_nestedp2m(struct domain *d)
{
    uint8_t i;
    struct p2m_domain *p2m;

    mm_lock_init(&d->arch.nested_p2m_lock);
    for (i = 0; i < MAX_NESTEDP2M; i++) {
        d->arch.nested_p2m[i] = p2m = xzalloc(struct p2m_domain);
        if (p2m == NULL)
            return -ENOMEM;
        if ( !zalloc_cpumask_var(&p2m->dirty_cpumask) )
            return -ENOMEM;
        p2m_initialise(d, p2m);
        p2m->write_p2m_entry = nestedp2m_write_p2m_entry;
        list_add(&p2m->np2m_list, &p2m_get_hostp2m(d)->np2m_list);
    }

    return 0;
}
#endif  /* __UXEN__ */

int p2m_init(struct domain *d)
{
    struct p2m_domain *p2m;
    int rc;

    p2m_get_hostp2m(d) = p2m = (struct p2m_domain *)d->extra_1->p2m;
    if ( !zalloc_cpumask_var(&p2m->dirty_cpumask) )
        return -ENOMEM;
    p2m_initialise(d, p2m);

#ifndef __UXEN__
    /* Must initialise nestedp2m unconditionally
     * since nestedhvm_enabled(d) returns false here.
     * (p2m_init runs too early for HVM_PARAM_* options) */
    rc = p2m_init_nestedp2m(d);
    if ( rc ) 
        p2m_final_teardown(d);
#else   /* __UXEN__ */
    rc = 0;
#endif  /* __UXEN__ */
    return rc;
}

void p2m_change_entry_type_global(struct domain *d,
                                  p2m_type_t ot, p2m_type_t nt)
{
    struct p2m_domain *p2m = p2m_get_hostp2m(d);
DEBUG();
    p2m_lock(p2m);
    p2m->change_entry_type_global(p2m, ot, nt);
    p2m_unlock(p2m);
}

mfn_t get_gfn_type_access(struct p2m_domain *p2m, unsigned long gfn,
                    p2m_type_t *t, p2m_access_t *a, p2m_query_t q,
                    unsigned int *page_order)
{
    mfn_t mfn;

    if ( !p2m || !paging_mode_translate(p2m->domain) )
    {
        /* Not necessarily true, but for non-translated guests, we claim
         * it's the most generic kind of memory */
        *t = p2m_ram_rw;
        return _mfn(gfn);
    }

    mfn = p2m->get_entry(p2m, gfn, t, a, q, page_order);

#ifndef __UXEN__
#ifdef __x86_64__
    if ( q == p2m_unshare && p2m_is_shared(*t) )
    {
#ifndef __UXEN__
        ASSERT(!p2m_is_nestedp2m(p2m));
#endif  /* __UXEN__ */
        mem_sharing_unshare_page(p2m->domain, gfn, 0);
        mfn = p2m->get_entry(p2m, gfn, t, a, q, page_order);
    }
#endif
#endif  /* __UXEN__ */

#ifndef __UXEN__
#ifdef __x86_64__
    if (unlikely((p2m_is_broken(*t))))
    {
        /* Return invalid_mfn to avoid caller's access */
        mfn = _mfn(INVALID_MFN);
        if (is_p2m_guest_query(q))
            domain_crash(p2m->domain);
    }
#endif
#endif  /* __UXEN__ */

    return mfn;
}

mfn_t
get_gfn_contents(struct domain *d, unsigned long gpfn, p2m_type_t *t,
                 uint8_t *buffer, uint32_t *size, int remove)
{
    struct p2m_domain *p2m = p2m_get_hostp2m(d);
    p2m_access_t a;
    unsigned int page_order;
    mfn_t mfn;
    struct page_info *page;
    void *s;
    uint8_t *data = NULL;
    int rc;

    *size = 0;

    rc = p2m_gfn_check_limit(d, gpfn, PAGE_ORDER_4K);
    if (rc)
        return _mfn(ERROR_MFN);

    p2m_lock(p2m);
    mfn = p2m->get_entry(p2m, gpfn, t, &a, p2m_query, &page_order);
    if (mfn_zero_page(mfn_x(mfn)) || is_xen_mfn(mfn_x(mfn)) ||
        is_host_mfn(mfn_x(mfn)))
        goto out;
    while (mfn_valid_page(mfn)) {
        page = mfn_to_page(mfn);
        if (unlikely(page_get_owner(mfn_to_page(mfn)) != d ||
                     !get_page(page, d)))
            /* if the page doesn't belong to this VM, then we don't
             * provide the contents */
            break;

        if (remove)
            guest_physmap_mark_pod_locked(d, gpfn, PAGE_ORDER_4K,
                                          _mfn(SHARED_ZERO_MFN));

        s = map_domain_page(mfn_x(mfn));
        memcpy(buffer, s, PAGE_SIZE);
        unmap_domain_page(s);
        *size = PAGE_SIZE;

        put_page(page);
        goto out;
    }
    while (p2m_is_pod(*t) && p2m_mfn_is_page_data(mfn_x(mfn))) {
        uint16_t offset;

        if (p2m_parse_page_data(&mfn, &data, &offset)) {
            mfn = _mfn(ERROR_MFN);
            goto out;
        }

        page = mfn_to_page(mfn);
        if (unlikely(page_get_owner(mfn_to_page(mfn)) != d ||
                     !get_page(page, d)))
            /* if the page storing the compressed data doesn't belong
             * to this VM, then we don't provide the contents */
            break;

        *(uint16_t *)buffer = PAGE_SIZE - sizeof(uint16_t);
        if (!p2m_get_compressed_page_data(
                d, mfn, data, offset,
                &buffer[sizeof(uint16_t)], (uint16_t *)buffer)) {
            mfn = _mfn(ERROR_MFN);
            put_page(page);
            goto out;
        }

        *size = sizeof(uint16_t) + *(uint16_t *)buffer;
        mfn = _mfn(COMPRESSED_MFN);

        put_page(page);
        goto out;
    }
    if (p2m_is_pod(*t)) {
        mfn = _mfn(INVALID_MFN);
        goto out;
    }
    mfn = _mfn(INVALID_MFN);

  out:
    if (data)
        unmap_domain_page_direct(data);
    p2m_unlock(p2m);
    return mfn;
}

int set_p2m_entry(struct p2m_domain *p2m, unsigned long gfn, mfn_t mfn, 
                  unsigned int page_order, p2m_type_t p2mt, p2m_access_t p2ma)
{
    struct domain *d = p2m->domain;
    unsigned long todo = 1ul << page_order;
    unsigned int order;
    int rc = 1;

    ASSERT(p2m_locked_by_me(p2m));

    while ( todo )
    {
        if ( hap_enabled(d) )
            order = ( (((gfn | mfn_x(mfn) | todo) & ((1ul << PAGE_ORDER_1G) - 1)) == 0) &&
                      hvm_hap_has_1gb(d) && opt_hap_1gb ) ? PAGE_ORDER_1G :
                      ((((gfn | mfn_x(mfn) | todo) & ((1ul << PAGE_ORDER_2M) - 1)) == 0) &&
                      hvm_hap_has_2mb(d) && opt_hap_2mb) ? PAGE_ORDER_2M : PAGE_ORDER_4K;
        else
            order = 0;

        if ( !p2m->set_entry(p2m, gfn, mfn, order, p2mt, p2ma) )
            rc = 0;
        gfn += 1ul << order;
        if (mfn_valid_page(mfn))
            mfn = _mfn(mfn_x(mfn) + (1ul << order));
        todo -= 1ul << order;
    }

    return rc;
}

struct page_info *p2m_alloc_ptp(struct p2m_domain *p2m, unsigned long type)
{
    struct page_info *pg;

    ASSERT(p2m);
    ASSERT(p2m->domain);
    ASSERT(p2m->domain->arch.paging.alloc_page);
    pg = p2m->domain->arch.paging.alloc_page(p2m->domain);
    if (pg == NULL)
        return NULL;

    page_list_add_tail(pg, &p2m->pages);
#ifndef __UXEN__
    pg->u.inuse.type_info = type | 1 | PGT_validated;
#endif  /* __UXEN__ */

    return pg;
}

void p2m_free_ptp(struct p2m_domain *p2m, struct page_info *pg)
{
    ASSERT(pg);
    ASSERT(p2m);
    ASSERT(p2m->domain);
    ASSERT(p2m->domain->arch.paging.free_page);

    page_list_del(pg, &p2m->pages);
    p2m->domain->arch.paging.free_page(p2m->domain, pg);

    return;
}

// Allocate a new p2m table for a domain.
//
// The structure of the p2m table is that of a pagetable for xen (i.e. it is
// controlled by CONFIG_PAGING_LEVELS).
//
// Returns 0 for success or -errno.
//
int p2m_alloc_table(struct p2m_domain *p2m)
{
#ifndef __UXEN__
    mfn_t mfn = _mfn(INVALID_MFN);
    struct page_info *page;
#endif  /* __UXEN__ */
    struct page_info *p2m_top;
#ifndef __UXEN__
    unsigned int page_count = 0;
    unsigned long gfn = -1UL;
#endif  /* __UXEN__ */
    struct domain *d = p2m->domain;

    p2m_lock(p2m);

    if ( pagetable_get_pfn(p2m_get_pagetable(p2m)) != 0 )
    {
        P2M_ERROR("p2m already allocated for this domain\n");
        p2m_unlock(p2m);
        return -EINVAL;
    }

    P2M_PRINTK("allocating p2m table\n");

    p2m_top = p2m_alloc_ptp(p2m,
#ifndef __UXEN__
#if CONFIG_PAGING_LEVELS == 4
        PGT_l4_page_table
#else
        PGT_l3_page_table
#endif
#else  /* __UXEN__ */
        0
#endif  /* __UXEN__ */
        );

    if ( p2m_top == NULL )
    {
        p2m_unlock(p2m);
        return -ENOMEM;
    }

    p2m->phys_table = pagetable_from_mfn(page_to_mfn(p2m_top));
    d->arch.hvm_domain.vmx.ept_control.asr  =
        pagetable_get_pfn(p2m_get_pagetable(p2m));

#ifndef __UXEN__
    if ( hap_enabled(d) )
        iommu_share_p2m_table(d);
#endif  /* __UXEN__ */

    P2M_PRINTK("populating p2m table\n");

    /* Initialise physmap tables for slot zero. Other code assumes this. */
#ifndef __UXEN__
    p2m->defer_nested_flush = 1;
#endif  /* __UXEN__ */
    if ( !set_p2m_entry(p2m, 0, _mfn(INVALID_MFN), PAGE_ORDER_4K,
                        p2m_invalid, p2m->default_access) )
        goto error;

#ifndef __UXEN__
    if ( !p2m_is_nestedp2m(p2m) )
    {
        /* Copy all existing mappings from the page list and m2p */
        spin_lock(&p2m->domain->page_alloc_lock);
        page_list_for_each(page, &p2m->domain->page_list)
        {
            mfn = page_to_mfn(page);
            gfn = get_gpfn_from_mfn(mfn_x(mfn));
            /* Pages should not be shared that early */
            ASSERT(gfn != SHARED_M2P_ENTRY);
            page_count++;
            if (
#ifdef __x86_64__
                (gfn != 0x5555555555555555L)
#else
                (gfn != 0x55555555L)
#endif
                && gfn != INVALID_M2P_ENTRY
                && !set_p2m_entry(p2m, gfn, mfn, PAGE_ORDER_4K, p2m_ram_rw, p2m->default_access) )
                goto error_unlock;
        }
        spin_unlock(&p2m->domain->page_alloc_lock);
    }
#endif  /* __UXEN__ */
#ifndef __UXEN__
    p2m->defer_nested_flush = 0;
#endif  /* __UXEN__ */

    P2M_PRINTK("p2m table initialised (%u pages)\n", page_count);
    p2m_unlock(p2m);
    return 0;

#ifndef __UXEN__
error_unlock:
#endif  /* __UXEN__ */
    spin_unlock(&p2m->domain->page_alloc_lock);
 error:
    P2M_PRINTK("failed to initialize p2m table, gfn=%05lx, mfn=%"
               PRI_mfn "\n", gfn, mfn_x(mfn));
    p2m_unlock(p2m);
    return -ENOMEM;
}

void p2m_teardown(struct p2m_domain *p2m)
/* Return all the p2m pages to Xen.
 * We know we don't have any extra mappings to these pages */
{
    struct page_info *pg;
    struct domain *d = p2m->domain;
#ifndef __UXEN__
#ifdef __x86_64__
    unsigned long gfn;
    p2m_type_t t;
    mfn_t mfn;
#endif
#endif  /* __UXEN__ */

    if (p2m == NULL)
        return;

    p2m_lock(p2m);

#ifndef __UXEN__
#ifdef __x86_64__
    for ( gfn=0; gfn <= p2m->max_mapped_pfn; gfn++ )
    {
        p2m_access_t a;
        mfn = p2m->get_entry(p2m, gfn, &t, &a, p2m_query, NULL);
        if (mfn_valid(mfn) && p2m_is_shared(t)) {
#ifndef __UXEN__
            ASSERT(!p2m_is_nestedp2m(p2m));
#endif  /* __UXEN__ */
            BUG_ON(mem_sharing_unshare_page(d, gfn, MEM_SHARING_DESTROY_GFN));
        }
    }
#endif
#endif  /* __UXEN__ */

    p2m_l1_cache_flush();

    p2m->phys_table = pagetable_null();

    while ( (pg = page_list_remove_head(&p2m->pages)) )
        d->arch.paging.free_page(d, pg);
    p2m_unlock(p2m);
}

#ifndef __UXEN__
static void p2m_teardown_nestedp2m(struct domain *d)
{
    uint8_t i;

DEBUG();
    for (i = 0; i < MAX_NESTEDP2M; i++) {
        if ( !d->arch.nested_p2m[i] )
            continue;
        free_cpumask_var(d->arch.nested_p2m[i]->dirty_cpumask);
        xfree(d->arch.nested_p2m[i]);
        d->arch.nested_p2m[i] = NULL;
    }
}
#endif  /* __UXEN__ */

void p2m_final_teardown(struct domain *d)
{
    /* Iterate over all p2m tables per domain */
    if ( d->arch.p2m )
    {
        free_cpumask_var(d->arch.p2m->dirty_cpumask);
        d->arch.p2m = NULL;
    }

#ifndef __UXEN__
    /* We must teardown unconditionally because
     * we initialise them unconditionally.
     */
    p2m_teardown_nestedp2m(d);
#endif  /* __UXEN__ */
}


static int
p2m_mapcache_map(struct domain *d, xen_pfn_t gpfn, mfn_t mfn)
{
    struct page_info *page;
    xen_pfn_t omfn;

    page = mfn_to_page(mfn);
    if (unlikely(!get_page(page, d))) {
        if (!d->is_dying)
            gdprintk(XENLOG_INFO,
                     "%s: mfn %lx for vm%u gpfn %"PRI_xen_pfn" vanished\n",
                     __FUNCTION__, mfn_x(mfn), d->domain_id, gpfn);
        return -EINVAL;
    }
    spin_lock(&d->page_alloc_lock);
    if (!test_and_set_bit(_PGC_mapcache, &page->count_info)) {
        page_list_del2(page, &d->page_list, &d->xenpage_list);
        page_list_add_tail(page, &d->mapcache_page_list);
    } else
        /* This happens when a range of pages is being mapped, and
         * some of those pages are already mapped -- mdm_enter detects
         * this and does nothing, returning an invalid omfn -- it does
         * however honour mdm->mdm_takeref, which is why we still call
         * it from here after this condition is detected. */
        put_page(page);
    omfn = mdm_enter(d, gpfn, mfn_x(mfn));
    if (__mfn_valid(omfn)) {
        page = __mfn_to_page(omfn);
        if (!test_and_clear_bit(_PGC_mapcache, &page->count_info))
            gdprintk(XENLOG_WARNING,
                     "%s: mfn %"PRI_xen_pfn" in mapcache for vm%u gpfn"
                     " %"PRI_xen_pfn" without _PGC_mapcache\n", __FUNCTION__,
                     omfn, d->domain_id, gpfn);
        else {
            page_list_del(page, &d->mapcache_page_list);
            page_list_add_tail(page, is_xen_page(page) ?
                               &d->xenpage_list : &d->page_list);
            put_page(page);
        }
    }
    spin_unlock(&d->page_alloc_lock);
    return 0;
}

static void
p2m_remove_page(struct p2m_domain *p2m, unsigned long gfn, unsigned long mfn,
                unsigned int page_order)
{
    unsigned long i;
    mfn_t mfn_return;
    p2m_type_t t;
    p2m_access_t a;

    if ( !paging_mode_translate(p2m->domain) )
    {
#ifndef __UXEN__
        if ( need_iommu(p2m->domain) )
            for ( i = 0; i < (1 << page_order); i++ )
                iommu_unmap_page(p2m->domain, mfn + i);
#endif  /* __UXEN__ */
        return;
    }

    if (p2m_debug_more)
    P2M_DEBUG("removing gfn=%#lx mfn=%#lx\n", gfn, mfn);

    if ( __mfn_valid(mfn) )
    {
        for ( i = 0; i < (1UL << page_order); i++ )
        {
            mfn_return = p2m->get_entry(p2m, gfn + i, &t, &a, p2m_query, NULL);
#ifndef __UXEN__
            if ( !p2m_is_grant(t) )
                set_gpfn_from_mfn(mfn+i, INVALID_M2P_ENTRY);
#endif  /* __UXEN__ */
            ASSERT( !p2m_is_valid(t) || mfn + i == mfn_x(mfn_return) );
            p2m_update_pod_counts(p2m->domain, mfn_x(mfn_return), t);
        }
    }
    set_p2m_entry(p2m, gfn, _mfn(INVALID_MFN), page_order, p2m_invalid, p2m->default_access);
}

void
guest_physmap_remove_page(struct domain *d, unsigned long gfn,
                          unsigned long mfn, unsigned int page_order)
{
    struct p2m_domain *p2m = p2m_get_hostp2m(d);
    p2m_lock(p2m);
    audit_p2m(p2m, 1);
    p2m_remove_page(p2m, gfn, mfn, page_order);
    audit_p2m(p2m, 1);
    p2m_unlock(p2m);
}

int
guest_physmap_add_entry(struct domain *d, unsigned long gfn,
                        unsigned long mfn, unsigned int page_order, 
                        p2m_type_t t)
{
    struct p2m_domain *p2m = p2m_get_hostp2m(d);
    unsigned long i;
#ifndef __UXEN__
    unsigned long ogfn;
#endif  /* __UXEN__ */
    p2m_type_t ot;
    p2m_access_t a;
    mfn_t omfn;
    int pod_count = 0, pod_zero_count = 0, pod_tmpl_count = 0;
    int rc = 0;

    if ( !paging_mode_translate(d) )
    {
#ifndef __UXEN__
        if (need_iommu(d) && p2m_is_ram_rw(t)) {
            for ( i = 0; i < (1 << page_order); i++ )
            {
                rc = iommu_map_page(
                    d, mfn + i, mfn + i, IOMMUF_readable|IOMMUF_writable);
                if ( rc != 0 )
                {
                    while ( i-- > 0 )
                        iommu_unmap_page(d, mfn + i);
                    return rc;
                }
            }
        }
#endif  /* __UXEN__ */
        return 0;
    }

    rc = p2m_gfn_check_limit(d, gfn, page_order);
    if ( rc != 0 )
        return rc;

    p2m_lock(p2m);
    audit_p2m(p2m, 0);

    if (p2m_debug_more)
    P2M_DEBUG("adding gfn=%#lx mfn=%#lx\n", gfn, mfn);

    /* First, remove m->p mappings for existing p->m mappings */
    for ( i = 0; i < (1UL << page_order); i++ )
    {
        omfn = p2m->get_entry(p2m, gfn + i, &ot, &a, p2m_query, NULL);
#ifndef __UXEN__
        if ( p2m_is_grant(ot) )
        {
            /* Really shouldn't be unmapping grant maps this way */
            domain_crash(d);
            p2m_unlock(p2m);
            
            return -EINVAL;
        }
        else
#endif  /* __UXEN__ */
        if (p2m_is_ram(ot)) {
            ASSERT(mfn_valid(omfn));
            if (test_bit(_PGC_mapcache, &mfn_to_page(omfn)->count_info) &&
                p2m_clear_gpfn_from_mapcache(p2m, gfn + i, omfn)) {
                int ret;
                /* caller beware that briefly the page seen through
                 * the userspace mapping is the new mapping while the
                 * old mapping is still present in the p2m -- ok since
                 * this operation is only supported while the VM is
                 * suspended */
                if (!d->is_shutting_down || !__mfn_valid(mfn)) {
                    if (__mfn_valid(mfn))
                        gdprintk(XENLOG_WARNING,
                                 "%s: can't clear mapcache mapped mfn %lx"
                                 " for vm%u gpfn %lx new mfn %lx\n",
                                 __FUNCTION__, mfn_x(omfn), d->domain_id,
                                 gfn + i, mfn);
                    domain_crash(d);
                    p2m_unlock(p2m);
                    return -EINVAL;
                }
                ret = p2m_mapcache_map(d, gfn + i, _mfn(mfn));
                if (ret) {
                    domain_crash(d);
                    p2m_unlock(p2m);
                    return -EINVAL;
                }
            }
#ifndef __UXEN__
            set_gpfn_from_mfn(mfn_x(omfn), INVALID_M2P_ENTRY);
#endif  /* __UXEN__ */
        }
        else if (p2m_is_pod(ot)) {
            /* Count how man PoD entries we'll be replacing if successful */
            if (mfn_x(omfn) == 0)
                pod_count++;
            else if (mfn_zero_page(mfn_x(omfn)))
                pod_zero_count++;
            else
                pod_tmpl_count++;
        }
    }

#ifndef __UXEN__
    /* Then, look for m->p mappings for this range and deal with them */
    for ( i = 0; i < (1UL << page_order); i++ )
    {
        if ( page_get_owner(__mfn_to_page(mfn + i)) != d )
            continue;
        ogfn = mfn_to_gfn(d, _mfn(mfn+i));
        if (
#if defined(__x86_64__) && !defined(__UXEN__)
            (ogfn != 0x5555555555555555L)
#else
            (ogfn != 0x55555555L)
#endif
            && (ogfn != INVALID_M2P_ENTRY)
            && (ogfn != gfn + i) )
        {
            /* This machine frame is already mapped at another physical
             * address */
            P2M_DEBUG("aliased! mfn=%#lx, old gfn=%#lx, new gfn=%#lx\n",
                      mfn + i, ogfn, gfn + i);
            omfn = p2m->get_entry(p2m, ogfn, &ot, &a, p2m_query, NULL);
            if ( p2m_is_ram(ot) )
            {
                ASSERT(mfn_valid(omfn));
                P2M_DEBUG("old gfn=%#lx -> mfn %#lx\n",
                          ogfn , mfn_x(omfn));
                if ( mfn_x(omfn) == (mfn + i) )
                    p2m_remove_page(p2m, ogfn, mfn + i, 0);
            }
        }
    }
#endif  /* __UXEN__ */

    /* Now, actually do the two-way mapping */
    if ( __mfn_valid(mfn) )
    {
        if ( !set_p2m_entry(p2m, gfn, _mfn(mfn), page_order, t, p2m->default_access) )
        {
            rc = -EINVAL;
            goto out; /* Failed to update p2m, bail without updating m2p. */
        }
#ifndef __UXEN__
        if ( !p2m_is_grant(t) )
        {
            for ( i = 0; i < (1UL << page_order); i++ )
                set_gpfn_from_mfn(mfn+i, gfn+i);
        }
#endif  /* __UXEN__ */
    }
    else
    {
        gdprintk(XENLOG_WARNING, "Adding bad mfn to p2m map (%#lx -> %#lx)\n",
                 gfn, mfn);
        if ( !set_p2m_entry(p2m, gfn, _mfn(INVALID_MFN), page_order, 
                            p2m_invalid, p2m->default_access) )
            rc = -EINVAL;
        else
        {
#ifndef __UXEN__
            p2m->pod.entry_count -= pod_count; /* Lock: p2m */
            BUG_ON(p2m->pod.entry_count < 0);
#else  /* __UXEN__ */
            atomic_sub(pod_count + pod_zero_count + pod_tmpl_count,
                &d->pod_pages); /* Lock: p2m */
            atomic_sub(pod_zero_count, &d->zero_shared_pages);
            atomic_sub(pod_tmpl_count, &d->tmpl_shared_pages);
#endif  /* __UXEN__ */
        }
    }

out:
    audit_p2m(p2m, 1);
    p2m_unlock(p2m);

    return rc;
}


/* Modify the p2m type of a single gfn from ot to nt, returning the 
 * entry's previous type.  Resets the access permissions. */
p2m_type_t p2m_change_type(struct domain *d, unsigned long gfn, 
                           p2m_type_t ot, p2m_type_t nt)
{
    p2m_access_t a;
    p2m_type_t pt;
    mfn_t mfn;
    struct p2m_domain *p2m = p2m_get_hostp2m(d);

#ifndef __UXEN__
    BUG_ON(p2m_is_grant(ot) || p2m_is_grant(nt));
#endif  /* __UXEN__ */

    p2m_lock(p2m);

    mfn = p2m->get_entry(p2m, gfn, &pt, &a, p2m_query, NULL);
    if ( pt == ot )
        set_p2m_entry(p2m, gfn, mfn, PAGE_ORDER_4K, nt, p2m->default_access);

    p2m_unlock(p2m);

    return pt;
}

/* Modify the p2m type of a range of gfns from ot to nt.
 * Resets the access permissions. */
void p2m_change_type_range(struct domain *d, 
                           unsigned long start, unsigned long end,
                           p2m_type_t ot, p2m_type_t nt)
{
    p2m_access_t a;
    p2m_type_t pt;
    unsigned long gfn;
    mfn_t mfn;
    struct p2m_domain *p2m = p2m_get_hostp2m(d);

#ifndef __UXEN__
    BUG_ON(p2m_is_grant(ot) || p2m_is_grant(nt));
#endif  /* __UXEN__ */

    p2m_lock(p2m);
#ifndef __UXEN__
    p2m->defer_nested_flush = 1;
#endif  /* __UXEN__ */

    for ( gfn = start; gfn < end; gfn++ )
    {
        mfn = p2m->get_entry(p2m, gfn, &pt, &a, p2m_query, NULL);
        if ( pt == ot )
            set_p2m_entry(p2m, gfn, mfn, PAGE_ORDER_4K, nt, p2m->default_access);
    }

#ifndef __UXEN__
    p2m->defer_nested_flush = 0;
    if ( nestedhvm_enabled(d) )
        p2m_flush_nestedp2m(d);
#endif  /* __UXEN__ */
    p2m_unlock(p2m);
}

/* Modify the p2m type of a range of gfns from ot to nt.
 * Resets the access permissions. */
void p2m_change_type_range_l2(struct domain *d, 
                              unsigned long start, unsigned long end,
                              p2m_type_t ot, p2m_type_t nt)
{
    unsigned long gfn;
    struct p2m_domain *p2m = p2m_get_hostp2m(d);
    int need_sync = 0;

#ifndef __UXEN__
    BUG_ON(p2m_is_grant(ot) || p2m_is_grant(nt));
#endif  /* __UXEN__ */

    p2m_lock(p2m);

    for ( gfn = start; gfn < end; gfn += (1ul << PAGE_ORDER_2M) )
        p2m->ro_update_l2_entry(p2m, gfn, p2m_is_logdirty(nt), &need_sync);

    if (need_sync)
        pt_sync_domain(p2m->domain);

    p2m_unlock(p2m);
}



int
set_mmio_p2m_entry(struct domain *d, unsigned long gfn, mfn_t mfn)
{
    int rc = 0;
    p2m_access_t a;
    p2m_type_t ot;
    mfn_t omfn;
    struct p2m_domain *p2m = p2m_get_hostp2m(d);

    if ( !paging_mode_translate(d) )
        return 0;

    p2m_lock(p2m);
    omfn = p2m->get_entry(p2m, gfn, &ot, &a, p2m_query, NULL);
#ifndef __UXEN__
    if ( p2m_is_grant(ot) )
    {
        p2m_unlock(p2m);
        domain_crash(d);
        return 0;
    }
    else
    if (p2m_is_ram(ot)) {
        ASSERT(mfn_valid(omfn));
        set_gpfn_from_mfn(mfn_x(omfn), INVALID_M2P_ENTRY);
    }
#endif  /* __UXEN__ */

    P2M_DEBUG("set mmio %lx %lx\n", gfn, mfn_x(mfn));
    rc = set_p2m_entry(p2m, gfn, mfn, PAGE_ORDER_4K, p2m_mmio_direct, p2m->default_access);
    p2m_update_pod_counts(p2m->domain, mfn_x(omfn), ot);
    audit_p2m(p2m, 1);
    p2m_unlock(p2m);
    if ( 0 == rc )
        gdprintk(XENLOG_ERR,
            "set_mmio_p2m_entry: set_p2m_entry failed! mfn=%08lx\n",
            mfn_x(get_gfn_query_unlocked(p2m->domain, gfn, &ot)));
    return rc;
}

int
clear_mmio_p2m_entry(struct domain *d, unsigned long gfn)
{
    int rc = 0;
    mfn_t mfn;
    p2m_access_t a;
    p2m_type_t t;
    struct p2m_domain *p2m = p2m_get_hostp2m(d);

DEBUG();
    if ( !paging_mode_translate(d) )
        return 0;

    p2m_lock(p2m);
    mfn = p2m->get_entry(p2m, gfn, &t, &a, p2m_query, NULL);

    /* Do not use mfn_valid() here as it will usually fail for MMIO pages. */
    if ((INVALID_MFN == mfn_x(mfn)) || (!p2m_is_mmio_direct(t))) {
        gdprintk(XENLOG_ERR,
            "clear_mmio_p2m_entry: gfn_to_mfn failed! gfn=%08lx\n", gfn);
        goto out;
    }
    rc = set_p2m_entry(p2m, gfn, _mfn(INVALID_MFN), PAGE_ORDER_4K, p2m_invalid, p2m->default_access);
    audit_p2m(p2m, 1);

out:
    p2m_unlock(p2m);

    return rc;
}

#ifndef __UXEN__
int
set_shared_p2m_entry(struct domain *d, unsigned long gfn, mfn_t mfn)
{
    struct p2m_domain *p2m = p2m_get_hostp2m(d);
    int rc = 0;
    p2m_access_t a;
    p2m_type_t ot;
    mfn_t omfn;

DEBUG();
    if ( !paging_mode_translate(p2m->domain) )
        return 0;

    p2m_lock(p2m);
    omfn = p2m->get_entry(p2m, gfn, &ot, &a, p2m_query, NULL);
    /* At the moment we only allow p2m change if gfn has already been made
     * sharable first */
    ASSERT(p2m_is_shared(ot));
    ASSERT(mfn_valid(omfn));
    /* XXX: M2P translations have to be handled properly for shared pages */
    set_gpfn_from_mfn(mfn_x(omfn), INVALID_M2P_ENTRY);

    P2M_DEBUG("set shared %lx %lx\n", gfn, mfn_x(mfn));
    rc = set_p2m_entry(p2m, gfn, mfn, PAGE_ORDER_4K, p2m_ram_shared, p2m->default_access);
    p2m_unlock(p2m);
    if ( 0 == rc )
        gdprintk(XENLOG_ERR,
            "set_shared_p2m_entry: set_p2m_entry failed! mfn=%08lx\n",
            mfn_x(get_gfn_query_unlocked(p2m->domain, gfn, &ot)));
    return rc;
}

#ifndef __UXEN__
#ifdef __x86_64__
/**
 * p2m_mem_paging_nominate - Mark a guest page as to-be-paged-out
 * @d: guest domain
 * @gfn: guest page to nominate
 *
 * Returns 0 for success or negative errno values if gfn is not pageable.
 *
 * p2m_mem_paging_nominate() is called by the pager and checks if a guest page
 * can be paged out. If the following conditions are met the p2mt will be
 * changed:
 * - the gfn is backed by a mfn
 * - the p2mt of the gfn is pageable
 * - the mfn is not used for IO
 * - the mfn has exactly one user and has no special meaning
 *
 * Once the p2mt is changed the page is readonly for the guest.  On success the
 * pager can write the page contents to disk and later evict the page.
 */
int p2m_mem_paging_nominate(struct domain *d, unsigned long gfn)
{
    struct page_info *page;
    struct p2m_domain *p2m = p2m_get_hostp2m(d);
    p2m_type_t p2mt;
    p2m_access_t a;
    mfn_t mfn;
    int ret;

DEBUG();
    p2m_lock(p2m);

    mfn = p2m->get_entry(p2m, gfn, &p2mt, &a, p2m_query, NULL);

    /* Check if mfn is valid */
    ret = -EINVAL;
    if ( !mfn_valid(mfn) )
        goto out;

    /* Check p2m type */
    ret = -EAGAIN;
    if ( !p2m_is_pageable(p2mt) )
        goto out;

    /* Check for io memory page */
    if ( is_iomem_page(mfn_x(mfn)) )
        goto out;

    /* Check page count and type */
    page = mfn_to_page(mfn);
    if ( (page->count_info & (PGC_count_mask | PGC_allocated)) !=
         (1 | PGC_allocated) )
        goto out;

#ifndef __UXEN__
    if ( (page->u.inuse.type_info & PGT_type_mask) != PGT_none )
        goto out;
#endif  /* __UXEN__ */

    /* Fix p2m entry */
    set_p2m_entry(p2m, gfn, mfn, PAGE_ORDER_4K, p2m_ram_paging_out, a);
    audit_p2m(p2m, 1);
    ret = 0;

 out:
    p2m_unlock(p2m);
    return ret;
}

/**
 * p2m_mem_paging_evict - Mark a guest page as paged-out
 * @d: guest domain
 * @gfn: guest page to evict
 *
 * Returns 0 for success or negative errno values if eviction is not possible.
 *
 * p2m_mem_paging_evict() is called by the pager and will free a guest page and
 * release it back to Xen. If the following conditions are met the page can be
 * freed:
 * - the gfn is backed by a mfn
 * - the gfn was nominated
 * - the mfn has still exactly one user and has no special meaning
 *
 * After successful nomination some other process could have mapped the page. In
 * this case eviction can not be done. If the gfn was populated before the pager
 * could evict it, eviction can not be done either. In this case the gfn is
 * still backed by a mfn.
 */
int p2m_mem_paging_evict(struct domain *d, unsigned long gfn)
{
    struct page_info *page;
    p2m_type_t p2mt;
    p2m_access_t a;
    mfn_t mfn;
    struct p2m_domain *p2m = p2m_get_hostp2m(d);
    int ret = -EINVAL;

DEBUG();
    p2m_lock(p2m);

    /* Get mfn */
    mfn = p2m->get_entry(p2m, gfn, &p2mt, &a, p2m_query, NULL);
    if ( unlikely(!mfn_valid(mfn)) )
        goto out;

    /* Allow only nominated pages */
    if (!p2m_is_paging_out(p2mt))
        goto out;

    ret = -EBUSY;
    /* Get the page so it doesn't get modified under Xen's feet */
    page = mfn_to_page(mfn);
    if ( unlikely(!get_page(page, d)) )
        goto out;

    /* Check page count and type once more */
    if ( (page->count_info & (PGC_count_mask | PGC_allocated)) !=
         (2 | PGC_allocated) )
        goto out_put;

#ifndef __UXEN__
    if ( (page->u.inuse.type_info & PGT_type_mask) != PGT_none )
        goto out_put;
#endif  /* __UXEN__ */

    /* Decrement guest domain's ref count of the page */
    if ( test_and_clear_bit(_PGC_allocated, &page->count_info) )
        put_page(page);

    /* Remove mapping from p2m table */
    set_p2m_entry(p2m, gfn, _mfn(INVALID_MFN), PAGE_ORDER_4K, p2m_ram_paged, a);
    audit_p2m(p2m, 1);

    /* Clear content before returning the page to Xen */
    scrub_one_page(page);

    /* Track number of paged gfns */
    atomic_inc(&d->paged_pages);

    ret = 0;

 out_put:
    /* Put the page back so it gets freed */
    put_page(page);

 out:
    p2m_unlock(p2m);
    return ret;
}

/**
 * p2m_mem_paging_drop_page - Tell pager to drop its reference to a paged page
 * @d: guest domain
 * @gfn: guest page to drop
 *
 * p2m_mem_paging_drop_page() will notify the pager that a paged-out gfn was
 * released by the guest. The pager is supposed to drop its reference of the
 * gfn.
 */
void p2m_mem_paging_drop_page(struct domain *d, unsigned long gfn)
{
    struct vcpu *v = current;
    mem_event_request_t req;

DEBUG();
    /* Check that there's space on the ring for this request */
    if ( mem_event_check_ring(d, &d->mem_paging) == 0)
    {
        /* Send release notification to pager */
        memset(&req, 0, sizeof(req));
        req.flags |= MEM_EVENT_FLAG_DROP_PAGE;
        req.gfn = gfn;
        req.vcpu_id = v->vcpu_id;

        mem_event_put_request(d, &d->mem_paging, &req);
    }
}

/**
 * p2m_mem_paging_populate - Tell pager to populete a paged page
 * @d: guest domain
 * @gfn: guest page in paging state
 *
 * p2m_mem_paging_populate() will notify the pager that a page in any of the
 * paging states needs to be written back into the guest.
 * This function needs to be called whenever gfn_to_mfn() returns any of the p2m
 * paging types because the gfn may not be backed by a mfn.
 *
 * The gfn can be in any of the paging states, but the pager needs only be
 * notified when the gfn is in the paging-out path (paging_out or paged).  This
 * function may be called more than once from several vcpus. If the vcpu belongs
 * to the guest, the vcpu must be stopped and the pager notified that the vcpu
 * was stopped. The pager needs to handle several requests for the same gfn.
 *
 * If the gfn is not in the paging-out path and the vcpu does not belong to the
 * guest, nothing needs to be done and the function assumes that a request was
 * already sent to the pager. In this case the caller has to try again until the
 * gfn is fully paged in again.
 */
void p2m_mem_paging_populate(struct domain *d, unsigned long gfn)
{
    struct vcpu *v = current;
    mem_event_request_t req;
    p2m_type_t p2mt;
    p2m_access_t a;
    mfn_t mfn;
    struct p2m_domain *p2m = p2m_get_hostp2m(d);

DEBUG();
    /* Check that there's space on the ring for this request */
    if ( mem_event_check_ring(d, &d->mem_paging) )
        return;

    memset(&req, 0, sizeof(req));
    req.type = MEM_EVENT_TYPE_PAGING;

    /* Fix p2m mapping */
    p2m_lock(p2m);
    mfn = p2m->get_entry(p2m, gfn, &p2mt, &a, p2m_query, NULL);
    /* Allow only nominated or evicted pages to enter page-in path */
    if (p2m_is_paging_out(p2mt) || p2m_is_paged(p2mt)) {
        /* Evict will fail now, tag this request for pager */
        if ( p2mt == p2m_ram_paging_out )
            req.flags |= MEM_EVENT_FLAG_EVICT_FAIL;

        set_p2m_entry(p2m, gfn, mfn, PAGE_ORDER_4K, p2m_ram_paging_in_start, a);
        audit_p2m(p2m, 1);
    }
    p2m_unlock(p2m);

    /* Pause domain if request came from guest and gfn has paging type */
    if (  p2m_is_paging(p2mt) && v->domain == d )
    {
        vcpu_pause_nosync(v);
        req.flags |= MEM_EVENT_FLAG_VCPU_PAUSED;
    }
    /* No need to inform pager if the gfn is not in the page-out path */
    else if ( p2mt != p2m_ram_paging_out && p2mt != p2m_ram_paged )
    {
        /* gfn is already on its way back and vcpu is not paused */
        mem_event_put_req_producers(&d->mem_paging);
        return;
    }

    /* Send request to pager */
    req.gfn = gfn;
    req.p2mt = p2mt;
    req.vcpu_id = v->vcpu_id;

    mem_event_put_request(d, &d->mem_paging, &req);
}
#endif  /* __UXEN__ */

/**
 * p2m_mem_paging_prep - Allocate a new page for the guest
 * @d: guest domain
 * @gfn: guest page in paging state
 *
 * p2m_mem_paging_prep() will allocate a new page for the guest if the gfn is
 * not backed by a mfn. It is called by the pager.
 * It is required that the gfn was already populated. The gfn may already have a
 * mfn if populate was called for  gfn which was nominated but not evicted. In
 * this case only the p2mt needs to be forwarded.
 */
int p2m_mem_paging_prep(struct domain *d, unsigned long gfn)
{
    struct page_info *page;
    p2m_type_t p2mt;
    p2m_access_t a;
    mfn_t mfn;
    struct p2m_domain *p2m = p2m_get_hostp2m(d);
    int ret;

DEBUG();
    p2m_lock(p2m);

    mfn = p2m->get_entry(p2m, gfn, &p2mt, &a, p2m_query, NULL);

    ret = -ENOENT;
    /* Allow only missing pages */
    if (!p2m_is_paging_in_start(p2mt))
        goto out;

    /* Allocate a page if the gfn does not have one yet */
    if ( !mfn_valid(mfn) )
    {
        /* Get a free page */
        ret = -ENOMEM;
        page = alloc_domheap_page(p2m->domain, 0);
        if ( unlikely(page == NULL) )
            goto out;
        mfn = page_to_mfn(page);
    }

    /* Fix p2m mapping */
    set_p2m_entry(p2m, gfn, mfn, PAGE_ORDER_4K, p2m_ram_paging_in, a);
    audit_p2m(p2m, 1);

    atomic_dec(&d->paged_pages);

    ret = 0;

 out:
    p2m_unlock(p2m);
    return ret;
}

/**
 * p2m_mem_paging_resume - Resume guest gfn and vcpus
 * @d: guest domain
 * @gfn: guest page in paging state
 *
 * p2m_mem_paging_resume() will forward the p2mt of a gfn to ram_rw and all
 * waiting vcpus will be unpaused again. It is called by the pager.
 * 
 * The gfn was previously either evicted and populated, or nominated and
 * populated. If the page was evicted the p2mt will be p2m_ram_paging_in. If
 * the page was just nominated the p2mt will be p2m_ram_paging_in_start because
 * the pager did not call p2m_mem_paging_prep().
 *
 * If the gfn was dropped the vcpu needs to be unpaused.
 */
void p2m_mem_paging_resume(struct domain *d)
{
    struct p2m_domain *p2m = p2m_get_hostp2m(d);
    mem_event_response_t rsp;
    p2m_type_t p2mt;
    p2m_access_t a;
    mfn_t mfn;

DEBUG();
    /* Pull the response off the ring */
    mem_event_get_response(&d->mem_paging, &rsp);

    /* Fix p2m entry if the page was not dropped */
    if ( !(rsp.flags & MEM_EVENT_FLAG_DROP_PAGE) )
    {
        p2m_lock(p2m);
        mfn = p2m->get_entry(p2m, rsp.gfn, &p2mt, &a, p2m_query, NULL);
        /* Allow only pages which were prepared properly, or pages which
         * were nominated but not evicted */
        if ( mfn_valid(mfn) && 
             (p2mt == p2m_ram_paging_in || p2mt == p2m_ram_paging_in_start) )
        {
            set_p2m_entry(p2m, rsp.gfn, mfn, PAGE_ORDER_4K, p2m_ram_rw, a);
            set_gpfn_from_mfn(mfn_x(mfn), rsp.gfn);
            audit_p2m(p2m, 1);
        }
        p2m_unlock(p2m);
    }

    /* Unpause domain */
    if ( rsp.flags & MEM_EVENT_FLAG_VCPU_PAUSED )
        vcpu_unpause(d->vcpu[rsp.vcpu_id]);

    /* Unpause any domains that were paused because the ring was full */
    mem_event_unpause_vcpus(d);
}

void p2m_mem_access_check(unsigned long gpa, bool_t gla_valid, unsigned long gla, 
                          bool_t access_r, bool_t access_w, bool_t access_x)
{
    struct vcpu *v = current;
    mem_event_request_t req;
    unsigned long gfn = gpa >> PAGE_SHIFT;
    struct domain *d = v->domain;    
    struct p2m_domain* p2m = p2m_get_hostp2m(d);
    int res;
    mfn_t mfn;
    p2m_type_t p2mt;
    p2m_access_t p2ma;
    
DEBUG();
    /* First, handle rx2rw conversion automatically */
    p2m_lock(p2m);
    mfn = p2m->get_entry(p2m, gfn, &p2mt, &p2ma, p2m_query, NULL);

    if ( access_w && p2ma == p2m_access_rx2rw ) 
    {
        p2m->set_entry(p2m, gfn, mfn, PAGE_ORDER_4K, p2mt, p2m_access_rw);
        p2m_unlock(p2m);
        return;
    }
    p2m_unlock(p2m);

    /* Otherwise, check if there is a memory event listener, and send the message along */
    res = mem_event_check_ring(d, &d->mem_access);
    if ( res < 0 ) 
    {
        /* No listener */
        if ( p2m->access_required ) 
        {
            printk(XENLOG_INFO 
                   "Memory access permissions failure, no mem_event listener: "
                   "pausing vm%u.%u\n", d->domain_id, v->vcpu_id);

            mem_event_mark_and_pause(v);
        }
        else
        {
            /* A listener is not required, so clear the access restrictions */
            p2m_lock(p2m);
            p2m->set_entry(p2m, gfn, mfn, PAGE_ORDER_4K, p2mt, p2m_access_rwx);
            p2m_unlock(p2m);
        }

        return;
    }
    else if ( res > 0 )
        return;  /* No space in buffer; VCPU paused */

    memset(&req, 0, sizeof(req));
    req.type = MEM_EVENT_TYPE_ACCESS;
    req.reason = MEM_EVENT_REASON_VIOLATION;

    /* Pause the current VCPU unconditionally */
    vcpu_pause_nosync(v);
    req.flags |= MEM_EVENT_FLAG_VCPU_PAUSED;    

    /* Send request to mem event */
    req.gfn = gfn;
    req.offset = gpa & ((1 << PAGE_SHIFT) - 1);
    req.gla_valid = gla_valid;
    req.gla = gla;
    req.access_r = access_r;
    req.access_w = access_w;
    req.access_x = access_x;
    
    req.vcpu_id = v->vcpu_id;

    mem_event_put_request(d, &d->mem_access, &req);

    /* VCPU paused, mem event request sent */
}

void p2m_mem_access_resume(struct p2m_domain *p2m)
{
    struct domain *d = p2m->domain;
    mem_event_response_t rsp;

DEBUG();
    mem_event_get_response(&d->mem_access, &rsp);

    /* Unpause domain */
    if ( rsp.flags & MEM_EVENT_FLAG_VCPU_PAUSED )
        vcpu_unpause(d->vcpu[rsp.vcpu_id]);

    /* Unpause any domains that were paused because the ring was full or no listener 
     * was available */
    mem_event_unpause_vcpus(d);
}


/* Set access type for a region of pfns.
 * If start_pfn == -1ul, sets the default access type */
int p2m_set_mem_access(struct domain *d, unsigned long start_pfn, 
                       uint32_t nr, hvmmem_access_t access) 
{
    struct p2m_domain *p2m = p2m_get_hostp2m(d);
    unsigned long pfn;
    p2m_access_t a, _a;
    p2m_type_t t;
    mfn_t mfn;
    int rc = 0;

    /* N.B. _not_ static: initializer depends on p2m->default_access */
    p2m_access_t memaccess[] = {
        p2m_access_n,
        p2m_access_r,
        p2m_access_w,
        p2m_access_rw,
        p2m_access_x,
        p2m_access_rx,
        p2m_access_wx,
        p2m_access_rwx,
        p2m_access_rx2rw,
        p2m->default_access,
    };

DEBUG();
    if ( access >= HVMMEM_access_default || access < 0 )
        return -EINVAL;

    a = memaccess[access];

    /* If request to set default access */
    if ( start_pfn == ~0ull ) 
    {
        p2m->default_access = a;
        return 0;
    }

    p2m_lock(p2m);
    for ( pfn = start_pfn; pfn < start_pfn + nr; pfn++ )
    {
        mfn = p2m->get_entry(p2m, pfn, &t, &_a, p2m_query, NULL);
        if ( p2m->set_entry(p2m, pfn, mfn, PAGE_ORDER_4K, t, a) == 0 )
        {
            rc = -ENOMEM;
            break;
        }
    }
    p2m_unlock(p2m);
    return rc;
}

/* Get access type for a pfn
 * If pfn == -1ul, gets the default access type */
int p2m_get_mem_access(struct domain *d, unsigned long pfn, 
                       hvmmem_access_t *access)
{
    struct p2m_domain *p2m = p2m_get_hostp2m(d);
    p2m_type_t t;
    p2m_access_t a;
    mfn_t mfn;

    static const hvmmem_access_t memaccess[] = {
        HVMMEM_access_n,
        HVMMEM_access_r,
        HVMMEM_access_w,
        HVMMEM_access_rw,
        HVMMEM_access_x,
        HVMMEM_access_rx,
        HVMMEM_access_wx,
        HVMMEM_access_rwx,
        HVMMEM_access_rx2rw
    };

DEBUG();
    /* If request to get default access */
    if ( pfn == ~0ull ) 
    {
        *access = memaccess[p2m->default_access];
        return 0;
    }

    mfn = p2m->get_entry(p2m, pfn, &t, &a, p2m_query, NULL);
    if ( mfn_x(mfn) == INVALID_MFN )
        return -ESRCH;
    
    if ( a >= ARRAY_SIZE(memaccess) || a < 0 )
        return -ERANGE;

    *access =  memaccess[a];
    return 0;
}


#endif /* __x86_64__ */
#endif  /* __UXEN__ */

#ifndef __UXEN__
static struct p2m_domain *
p2m_getlru_nestedp2m(struct domain *d, struct p2m_domain *p2m)
{
    struct list_head *lru_list = &p2m_get_hostp2m(d)->np2m_list;
    
    ASSERT(!list_empty(lru_list));

    if ( p2m == NULL )
        p2m = list_entry(lru_list->prev, struct p2m_domain, np2m_list);

    list_move(&p2m->np2m_list, lru_list);

    return p2m;
}

/* Reset this p2m table to be empty */
static void
p2m_flush_table(struct p2m_domain *p2m)
{
    struct page_info *top, *pg;
    struct domain *d = p2m->domain;
    void *p;

    p2m_lock(p2m);

    /* "Host" p2m tables can have shared entries &c that need a bit more 
     * care when discarding them */
    ASSERT(p2m_is_nestedp2m(p2m));
    ASSERT(page_list_empty(&p2m->pod.super));
    ASSERT(page_list_empty(&p2m->pod.single));

    /* This is no longer a valid nested p2m for any address space */
    p2m->cr3 = CR3_EADDR;
    
    /* Zap the top level of the trie */
    top = mfn_to_page(pagetable_get_mfn(p2m_get_pagetable(p2m)));
    p = __map_domain_page(top);
    clear_page(p);
    unmap_domain_page(p);

    /* Make sure nobody else is using this p2m table */
    nestedhvm_vmcx_flushtlb(p2m);

    /* Free the rest of the trie pages back to the paging pool */
    while ( (pg = page_list_remove_head(&p2m->pages)) )
        if ( pg != top ) 
            d->arch.paging.free_page(d, pg);
    page_list_add(top, &p2m->pages);

    p2m_unlock(p2m);
}

void
p2m_flush(struct vcpu *v, struct p2m_domain *p2m)
{
    ASSERT(v->domain == p2m->domain);
    vcpu_nestedhvm(v).nv_p2m = NULL;
    p2m_flush_table(p2m);
    hvm_asid_flush_vcpu(v);
}

void
p2m_flush_nestedp2m(struct domain *d)
{
    int i;
    for ( i = 0; i < MAX_NESTEDP2M; i++ )
        p2m_flush_table(d->arch.nested_p2m[i]);
}

struct p2m_domain *
p2m_get_nestedp2m(struct vcpu *v, uint64_t cr3)
{
    /* Use volatile to prevent gcc to cache nv->nv_p2m in a cpu register as
     * this may change within the loop by an other (v)cpu.
     */
    volatile struct nestedvcpu *nv = &vcpu_nestedhvm(v);
    struct domain *d;
    struct p2m_domain *p2m;

    /* Mask out low bits; this avoids collisions with CR3_EADDR */
    cr3 &= ~(0xfffull);

    if (nv->nv_flushp2m && nv->nv_p2m) {
        nv->nv_p2m = NULL;
    }

    d = v->domain;
    nestedp2m_lock(d);
    p2m = nv->nv_p2m;
    if ( p2m ) 
    {
        p2m_lock(p2m);
        if ( p2m->cr3 == cr3 || p2m->cr3 == CR3_EADDR )
        {
            nv->nv_flushp2m = 0;
            p2m_getlru_nestedp2m(d, p2m);
            nv->nv_p2m = p2m;
            if (p2m->cr3 == CR3_EADDR)
                hvm_asid_flush_vcpu(v);
            p2m->cr3 = cr3;
            cpumask_set_cpu(v->processor, p2m->dirty_cpumask);
            p2m_unlock(p2m);
            nestedp2m_unlock(d);
            return p2m;
        }
        p2m_unlock(p2m);
    }

    /* All p2m's are or were in use. Take the least recent used one,
     * flush it and reuse. */
    p2m = p2m_getlru_nestedp2m(d, NULL);
    p2m_flush_table(p2m);
    p2m_lock(p2m);
    nv->nv_p2m = p2m;
    p2m->cr3 = cr3;
    nv->nv_flushp2m = 0;
    hvm_asid_flush_vcpu(v);
    cpumask_set_cpu(v->processor, p2m->dirty_cpumask);
    p2m_unlock(p2m);
    nestedp2m_unlock(d);

    return p2m;
}
#endif  /* __UXEN__ */

struct p2m_domain *
p2m_get_p2m(struct vcpu *v)
{
#ifndef __UXEN__
    if (nestedhvm_is_n2(v))
        return p2m_get_nestedp2m(v, nhvm_vcpu_hostcr3(v));
#endif  /* __UXEN__ */

DEBUG();
    return p2m_get_hostp2m(v->domain);

}

unsigned long paging_gva_to_gfn(struct vcpu *v,
                                unsigned long va,
                                uint32_t *pfec)
{
    struct p2m_domain *hostp2m = p2m_get_hostp2m(v->domain);
    const struct paging_mode *hostmode = paging_get_hostmode(v);

#ifndef __UXEN__
    if ( is_hvm_domain(v->domain)
        && paging_mode_hap(v->domain) 
        && nestedhvm_is_n2(v) )
    {
        unsigned long gfn;
        struct p2m_domain *p2m;
        const struct paging_mode *mode;
        uint64_t ncr3 = nhvm_vcpu_hostcr3(v);

        /* translate l2 guest va into l2 guest gfn */
        p2m = p2m_get_nestedp2m(v, ncr3);
        mode = paging_get_nestedmode(v);
        gfn = mode->gva_to_gfn(v, p2m, va, pfec);

        /* translate l2 guest gfn into l1 guest gfn */
        return hostmode->p2m_ga_to_gfn(v, hostp2m, ncr3,
                                       gfn << PAGE_SHIFT, pfec, NULL);
    }
#endif  /* __UXEN__ */

    return hostmode->gva_to_gfn(v, hostp2m, va, pfec);
}

int
p2m_translate(struct domain *d, xen_pfn_t *arr, int nr, int write, int map)
{
    struct p2m_domain *p2m;
    p2m_type_t pt;
    mfn_t mfn;
    int j;
    int rc;

    p2m = p2m_get_hostp2m(d);

    p2m_lock(p2m);
    for ( j = 0; j < nr; j++ ) {
        switch (write) {
        case 0:
            /* p2m_alloc_r, fill pod mappings, leave cow mappings as is */
            mfn = get_gfn_type(d, arr[j], &pt, p2m_alloc_r);
            break;
        case 1:
            /* p2m_unshare implies p2m_alloc, break pod/cow mappings */
            mfn = get_gfn_unshare(d, arr[j], &pt);
            break;
        default:
            rc = -EINVAL;
            goto out;
        }
        if (mfn_retry(mfn)) {
            rc = j;
            goto out;
        }
        if (map && !mfn_valid(mfn)) {
            gdprintk(XENLOG_INFO,
                     "Translate failed for vm%u page %"PRI_xen_pfn"\n",
                     d->domain_id, arr[j]);
            rc = -EINVAL;
            goto out;
        }
        if (unlikely(is_xen_mfn(mfn_x(mfn))) ||
            unlikely(is_host_mfn(mfn_x(mfn))) ||
            unlikely(mfn_zero_page(mfn_x(mfn))))
            /* don't allow p2m_translate access to xen pages or host pages */
            mfn = _mfn(INVALID_MFN);
        else if (map) {
            rc = p2m_mapcache_map(d, arr[j], mfn);
            if (rc)
                goto out;
        } else if (mfn_valid(mfn))  {
            if (!write && p2m_is_pod(pt)) {
                /* Populate on demand: cloned shared page. */
                struct page_info *page = mfn_to_page(mfn);
                ASSERT(d->clone_of == page_get_owner(page));
                if (!get_page(page, page_get_owner(page)))
                    DEBUG();
            } else if (!get_page(mfn_to_page(mfn), d))
                DEBUG();
        }
        put_gfn(d, arr[j]);
        arr[j] = mfn_x(mfn);
    }
    rc = j;
 out:
    p2m_unlock(p2m);
    return rc;
}

int
p2m_mapcache_mappings_teardown(struct domain *d)
{
    struct page_info *page;
    unsigned long mfn;
    int total = 0, bad = 0;

    if (!d->vm_info_shared)
        return 0;

    if (d->vm_info_shared->vmi_mapcache_active)
        return -EAGAIN;

    spin_lock_recursive(&d->page_alloc_lock);

    while ( (page = page_list_remove_head(&d->mapcache_page_list)) ) {
        mfn = __page_to_mfn(page);
        if (!test_and_clear_bit(_PGC_mapcache, &page->count_info) && bad < 5) {
            gdprintk(XENLOG_WARNING,
                     "Bad mapcache clear for page %lx in vm%u\n",
                     mfn, d->domain_id);
            bad++;
        }

        total++;
        page_list_add_tail(page, is_xen_page(page) ?
                           &d->xenpage_list : &d->page_list);
        put_page(page);
    }

    gdprintk(XENLOG_INFO, "%s: total %d in vm%u, %d bad\n",
             __FUNCTION__, total, d->domain_id, bad);
    spin_unlock_recursive(&d->page_alloc_lock);

    return 0;
}

DEFINE_PER_CPU(union p2m_l1_cache, p2m_l1_cache);
atomic_t p2m_l1_cache_gen = ATOMIC_INIT(0);

static void
_p2m_l1_cache_flush(union p2m_l1_cache *l1c)
{
    int j;

    l1c->se_l1_mfn = _mfn(0);
    for (j = 0; j < NR_GE_L1_CACHE; j++)
        l1c->ge_l1_mfn[j] = _mfn(0);
}

static void
p2m_l1_cache_flush(void)
{
    uint16_t oldgen;

    oldgen = atomic_read(&p2m_l1_cache_gen);
    atomic_inc(&p2m_l1_cache_gen);
    if ((oldgen ^ _atomic_read(p2m_l1_cache_gen)) &
        ((P2M_L1_CACHE_GEN_MASK + 1) >> 1))
        cpumask_raise_softirq(&cpu_online_map, P2M_L1_CACHE_SOFTIRQ);
}

void
p2m_l1_cache_flush_softirq(void)
{

    _p2m_l1_cache_flush(&this_cpu(p2m_l1_cache));
}

/* Non-l1 update -- invalidate the get_entry cache */
void
p2m_ge_l1_cache_invalidate(struct p2m_domain *p2m, unsigned long gfn,
                           unsigned int page_order)
{
    /* flush all per-cpu caches unconditionally */
    p2m_l1_cache_flush();

    perfc_incr(p2m_get_entry_invalidate);
}

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
