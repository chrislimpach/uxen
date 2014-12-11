/******************************************************************************
 * arch/x86/mm/p2m-pod.c
 *
 * Populate-on-demand p2m entries. 
 *
 * Copyright (c) 2009-2011 Citrix Systems, Inc.
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
 * Copyright 2011-2015, Bromium, Inc.
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
#include <xen/iommu.h>
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

/* Override macros from asm/page.h to make them work with mfn_t */
#undef mfn_to_page
#define mfn_to_page(_m) __mfn_to_page(mfn_x(_m))
#undef mfn_valid
#define mfn_valid(_mfn) __mfn_valid(mfn_x(_mfn))
#undef page_to_mfn
#define page_to_mfn(_pg) _mfn(__page_to_mfn(_pg))

#ifndef __UXEN__
#define superpage_aligned(_x)  (((_x)&(SUPERPAGE_PAGES-1))==0)
#endif  /* __UXEN__ */

static int
p2m_clone_l1(struct p2m_domain *op2m, struct p2m_domain *p2m,
             unsigned long gpfn, void *entry);

/* Enforce lock ordering when grabbing the "external" page_alloc lock */
static inline void lock_page_alloc(struct p2m_domain *p2m)
{
    page_alloc_mm_pre_lock();
    spin_lock(&(p2m->domain->page_alloc_lock));
    page_alloc_mm_post_lock(p2m->domain->arch.page_alloc_unlock_level);
}

static inline void unlock_page_alloc(struct p2m_domain *p2m)
{
    page_alloc_mm_unlock(p2m->domain->arch.page_alloc_unlock_level);
    spin_unlock(&(p2m->domain->page_alloc_lock));
}

#ifndef __UXEN__
/*
 * Populate-on-demand functionality
 */

static int
p2m_pod_cache_add(struct p2m_domain *p2m,
                  struct page_info *page,
                  unsigned long order)
{
    int i;
    struct page_info *p;
    struct domain *d = p2m->domain;

#ifndef NDEBUG
    mfn_t mfn;

    mfn = page_to_mfn(page);

    /* Check to make sure this is a contiguous region */
    if( mfn_x(mfn) & ((1 << order) - 1) )
    {
        printk("%s: mfn %lx not aligned order %lu! (mask %lx)\n",
               __func__, mfn_x(mfn), order, ((1UL << order) - 1));
        return -1;
    }
    
    for(i=0; i < 1 << order ; i++) {
        struct domain * od;

        p = mfn_to_page(_mfn(mfn_x(mfn) + i));
        od = page_get_owner(p);
        if(od != d)
        {
            printk("%s: mfn %lx expected owner d%d, got owner d%d!\n",
                   __func__, mfn_x(mfn), d->domain_id,
                   od?od->domain_id:-1);
            return -1;
        }
    }
#endif

    ASSERT(p2m_locked_by_me(p2m));

#ifndef __UXEN__
    /*
     * Pages from domain_alloc and returned by the balloon driver aren't
     * guaranteed to be zero; but by reclaiming zero pages, we implicitly
     * promise to provide zero pages. So we scrub pages before using.
     */
    for ( i = 0; i < (1 << order); i++ )
    {
        char *b = map_domain_page_direct(mfn_x(page_to_mfn(page)) + i);
        clear_page(b);
        unmap_domain_page_direct(b);
    }
#endif  /* __UXEN__ */

    lock_page_alloc(p2m);

    /* First, take all pages off the domain list */
    for(i=0; i < 1 << order ; i++)
    {
        p = page + i;
        page_list_del(p, &d->page_list);
    }

    /* Then add the first one to the appropriate populate-on-demand list */
    switch(order)
    {
    case PAGE_ORDER_2M:
        page_list_add_tail(page, &p2m->pod.super); /* lock: page_alloc */
        p2m->pod.count += 1 << order;
        break;
    case PAGE_ORDER_4K:
        page_list_add_tail(page, &p2m->pod.single); /* lock: page_alloc */
        p2m->pod.count += 1;
        break;
    default:
        BUG();
    }

    /* Ensure that the PoD cache has never been emptied.  
     * This may cause "zombie domains" since the page will never be freed. */
    BUG_ON( d->arch.relmem != RELMEM_not_started );

    unlock_page_alloc(p2m);

    return 0;
}

static int
p2m_pod_cache_add_zero(struct p2m_domain *p2m,
		       struct page_info *page,
		       unsigned long order)
{
    int i;

    for (i = 0; i < (1 << order); i++) {
	char *b = map_domain_page_direct(mfn_x(page_to_mfn(page)) + i);
	clear_page(b);
	unmap_domain_page_direct(b);
    }

    return p2m_pod_cache_add(p2m, page, order);
}

/* Get a page of size order from the populate-on-demand cache.  Will break
 * down 2-meg pages into singleton pages automatically.  Returns null if
 * a superpage is requested and no superpages are available.  Must be called
 * with the d->page_lock held. */
static struct page_info * p2m_pod_cache_get(struct p2m_domain *p2m,
                                            unsigned long order)
{
    struct page_info *p = NULL;
    int i;

    if ( order == PAGE_ORDER_2M && page_list_empty(&p2m->pod.super) )
    {
        return NULL;
    }
    else if ( order == PAGE_ORDER_4K && page_list_empty(&p2m->pod.single) )
    {
        unsigned long mfn;
        struct page_info *q;

        BUG_ON( page_list_empty(&p2m->pod.super) );

        /* Break up a superpage to make single pages. NB count doesn't
         * need to be adjusted. */
        p = page_list_remove_head(&p2m->pod.super);
        mfn = mfn_x(page_to_mfn(p));

        for ( i=0; i<SUPERPAGE_PAGES; i++ )
        {
            q = mfn_to_page(_mfn(mfn+i));
            page_list_add_tail(q, &p2m->pod.single);
        }
    }

    switch ( order )
    {
    case PAGE_ORDER_2M:
        BUG_ON( page_list_empty(&p2m->pod.super) );
        p = page_list_remove_head(&p2m->pod.super);
        p2m->pod.count -= 1 << order; /* Lock: page_alloc */
        break;
    case PAGE_ORDER_4K:
        BUG_ON( page_list_empty(&p2m->pod.single) );
        p = page_list_remove_head(&p2m->pod.single);
        p2m->pod.count -= 1;
        break;
    default:
        BUG();
    }

    /* Put the pages back on the domain page_list */
    for ( i = 0 ; i < (1 << order); i++ )
    {
        BUG_ON(page_get_owner(p + i) != p2m->domain);
        page_list_add_tail(p + i, &p2m->domain->page_list);
    }

    return p;
}

/* Set the size of the cache, allocating or freeing as necessary. */
static int
p2m_pod_set_cache_target(struct p2m_domain *p2m, unsigned long pod_target, int preemptible)
{
    struct domain *d = p2m->domain;
    int ret = 0;

    /* Increasing the target */
    while ( pod_target > p2m->pod.count )
    {
        struct page_info * page;
        int order;

#ifndef __UXEN__
        if ( (pod_target - p2m->pod.count) >= SUPERPAGE_PAGES )
            order = PAGE_ORDER_2M;
        else
#endif  /* __UXEN__ */
            order = PAGE_ORDER_4K;
    retry:
        page = alloc_domheap_pages(d, order, PAGE_ORDER_4K);
        if ( unlikely(page == NULL) )
        {
            if ( order == PAGE_ORDER_2M )
            {
                /* If we can't allocate a superpage, try singleton pages */
                order = PAGE_ORDER_4K;
                goto retry;
            }   
            
            printk("%s: Unable to allocate domheap page for pod cache.  target %lu cachesize %d\n",
                   __func__, pod_target, p2m->pod.count);
            ret = -ENOMEM;
            goto out;
        }

        p2m_pod_cache_add(p2m, page, order);

        if ( hypercall_preempt_check() && preemptible )
        {
            ret = -EAGAIN;
            goto out;
        }
    }

    /* Decreasing the target */
    /* We hold the p2m lock here, so we don't need to worry about
     * cache disappearing under our feet. */
    while ( pod_target < p2m->pod.count )
    {
        struct page_info * page;
        int order, i;

        /* Grab the lock before checking that pod.super is empty, or the last
         * entries may disappear before we grab the lock. */
        lock_page_alloc(p2m);

#ifndef __UXEN__
        if ( (p2m->pod.count - pod_target) > SUPERPAGE_PAGES
             && !page_list_empty(&p2m->pod.super) )
            order = PAGE_ORDER_2M;
        else
#endif  /* __UXEN__ */
            order = PAGE_ORDER_4K;

        page = p2m_pod_cache_get(p2m, order);

        ASSERT(page != NULL);

        unlock_page_alloc(p2m);

        /* Then free them */
        for ( i = 0 ; i < (1 << order) ; i++ )
        {
            /* Copied from common/memory.c:guest_remove_page() */
            if ( unlikely(!get_page(page+i, d)) )
            {
                gdprintk(XENLOG_INFO, "Bad page free for domain %u\n", d->domain_id);
                ret = -EINVAL;
                goto out;
            }

#ifndef __UXEN__
            if ( test_and_clear_bit(_PGT_pinned, &(page+i)->u.inuse.type_info) )
                put_page_and_type(page+i);
#endif  /* __UXEN__ */
            
            if ( test_and_clear_bit(_PGC_allocated, &(page+i)->count_info) )
                put_page(page+i);

            put_page(page+i);

            if ( hypercall_preempt_check() && preemptible )
            {
                ret = -EAGAIN;
                goto out;
            }
        }
    }

out:
    return ret;
}
#else  /* __UXEN__ */
static void
_p2m_pod_free_pages(struct domain *d)
{
    struct p2m_domain *p2m = p2m_get_hostp2m(d);
    struct page_info *p;

    lock_page_alloc(p2m);

    while ((p = page_list_remove_head(&d->pod_free_list))) {
        page_list_add_tail(p, &d->page_list);
        unlock_page_alloc(p2m);

        /* Copied from common/memory.c:guest_remove_page() */
        if (unlikely(!get_page(p, d))) {
            gdprintk(XENLOG_INFO, "Bad page free for domain %u\n",
                     d->domain_id);
            goto proceed;
        }

#ifndef __UXEN__
        if (test_and_clear_bit(_PGT_pinned, &p->u.inuse.type_info))
            put_page_and_type(p);
#endif  /* __UXEN__ */
            
        if (test_and_clear_bit(_PGC_allocated, &p->count_info))
            put_page(p);

        put_page(p);

      proceed:
        lock_page_alloc(p2m);
    }

    unlock_page_alloc(p2m);
}

void
p2m_pod_free_pages(struct domain *d)
{
    struct p2m_domain *p2m = p2m_get_hostp2m(d);

    p2m_lock(p2m);

    /* Don't do anything if the domain is being torn down */
    if (d->is_dying)
        goto out;

    _p2m_pod_free_pages(d);

out:
    p2m_unlock(p2m);
}

void
p2m_pod_final_free_pages(struct domain *d)
{
    struct p2m_domain *p2m = p2m_get_hostp2m(d);

    p2m_lock(p2m);

    ASSERT(d->is_dying);

    _p2m_pod_free_pages(d);

    p2m_unlock(p2m);
}
#endif  /* __UXEN__ */

#ifndef __UXEN__
/*
 * The "right behavior" here requires some careful thought.  First, some
 * definitions:
 * + M: static_max
 * + B: number of pages the balloon driver has ballooned down to.
 * + P: Number of populated pages. 
 * + T: Old target
 * + T': New target
 *
 * The following equations should hold:
 *  0 <= P <= T <= B <= M
 *  d->arch.p2m->pod.entry_count == B - P
 *  d->tot_pages == P + d->arch.p2m->pod.count
 *
 * Now we have the following potential cases to cover:
 *     B <T': Set the PoD cache size equal to the number of outstanding PoD
 *   entries.  The balloon driver will deflate the balloon to give back
 *   the remainder of the ram to the guest OS.
 *  T <T'<B : Increase PoD cache size.
 *  T'<T<=B : Here we have a choice.  We can decrease the size of the cache,
 *   get the memory right away.  However, that means every time we 
 *   reduce the memory target we risk the guest attempting to populate the 
 *   memory before the balloon driver has reached its new target.  Safer to
 *   never reduce the cache size here, but only when the balloon driver frees 
 *   PoD ranges.
 *
 * If there are many zero pages, we could reach the target also by doing
 * zero sweeps and marking the ranges PoD; but the balloon driver will have
 * to free this memory eventually anyway, so we don't actually gain that much
 * by doing so.
 *
 * NB that the equation (B<T') may require adjustment to the cache
 * size as PoD pages are freed as well; i.e., freeing a PoD-backed
 * entry when pod.entry_count == pod.count requires us to reduce both
 * pod.entry_count and pod.count.
 */
int
p2m_pod_set_mem_target(struct domain *d, unsigned long target)
{
    unsigned pod_target;
    struct p2m_domain *p2m = p2m_get_hostp2m(d);
    int ret = 0;
    unsigned long populated;

    p2m_lock(p2m);

    /* P == B: Nothing to do. */
    if ( p2m->pod.entry_count == 0 )
        goto out;

    /* Don't do anything if the domain is being torn down */
    if ( d->is_dying )
        goto out;

    /* T' < B: Don't reduce the cache size; let the balloon driver
     * take care of it. */
    if ( target < d->tot_pages )
        goto out;

    populated  = d->tot_pages - p2m->pod.count;

    pod_target = target - populated;

    /* B < T': Set the cache size equal to # of outstanding entries,
     * let the balloon driver fill in the rest. */
    if ( pod_target > p2m->pod.entry_count )
        pod_target = p2m->pod.entry_count;

    ASSERT( pod_target >= p2m->pod.count );

    ret = p2m_pod_set_cache_target(p2m, pod_target, 1/*preemptible*/);

out:
    p2m_unlock(p2m);

    return ret;
}

void
p2m_pod_empty_cache(struct domain *d)
{
    struct p2m_domain *p2m = p2m_get_hostp2m(d);
    struct page_info *page;

    /* After this barrier no new PoD activities can happen. */
    BUG_ON(!d->is_dying);
    spin_barrier(&p2m->lock.lock);

    lock_page_alloc(p2m);

    while ( (page = page_list_remove_head(&p2m->pod.super)) )
    {
        int i;
            
        for ( i = 0 ; i < SUPERPAGE_PAGES ; i++ )
        {
            BUG_ON(page_get_owner(page + i) != d);
            page_list_add_tail(page + i, &d->page_list);
        }

        p2m->pod.count -= SUPERPAGE_PAGES;
    }

    while ( (page = page_list_remove_head(&p2m->pod.single)) )
    {
        BUG_ON(page_get_owner(page) != d);
        page_list_add_tail(page, &d->page_list);

        p2m->pod.count -= 1;
    }

    BUG_ON(p2m->pod.count != 0);

    unlock_page_alloc(p2m);
}

int
p2m_pod_offline_or_broken_hit(struct page_info *p)
{
    struct domain *d;
    struct p2m_domain *p2m;
    struct page_info *q, *tmp;
    unsigned long mfn, bmfn;

    if ( !(d = page_get_owner(p)) || !(p2m = p2m_get_hostp2m(d)) )
        return 0;

    lock_page_alloc(p2m);
    bmfn = mfn_x(page_to_mfn(p));
    page_list_for_each_safe(q, tmp, &p2m->pod.super)
    {
        mfn = mfn_x(page_to_mfn(q));
        if ( (bmfn >= mfn) && ((bmfn - mfn) < SUPERPAGE_PAGES) )
        {
            unsigned long i;
            page_list_del(q, &p2m->pod.super);
            for ( i = 0; i < SUPERPAGE_PAGES; i++)
            {
                q = mfn_to_page(_mfn(mfn + i));
                page_list_add_tail(q, &p2m->pod.single);
            }
            page_list_del(p, &p2m->pod.single);
            p2m->pod.count--;
            goto pod_hit;
        }
    }

    page_list_for_each_safe(q, tmp, &p2m->pod.single)
    {
        mfn = mfn_x(page_to_mfn(q));
        if ( mfn == bmfn )
        {
            page_list_del(p, &p2m->pod.single);
            p2m->pod.count--;
            goto pod_hit;
        }
    }

    unlock_page_alloc(p2m);
    return 0;

pod_hit:
    page_list_add_tail(p, &d->arch.relmem_list);
    unlock_page_alloc(p2m);
    return 1;
}

void
p2m_pod_offline_or_broken_replace(struct page_info *p)
{
    struct domain *d;
    struct p2m_domain *p2m;

    if ( !(d = page_get_owner(p)) || !(p2m = p2m_get_hostp2m(d)) )
        return;

    free_domheap_page(p);

    p = alloc_domheap_page(d, PAGE_ORDER_4K);
    if ( unlikely(!p) )
        return;

    p2m_lock(p2m);
    p2m_pod_cache_add(p2m, p, PAGE_ORDER_4K);
    p2m_unlock(p2m);
    return;
}

/* This function is needed for two reasons:
 * + To properly handle clearing of PoD entries
 * + To "steal back" memory being freed for the PoD cache, rather than
 *   releasing it.
 *
 * Once both of these functions have been completed, we can return and
 * allow decrease_reservation() to handle everything else.
 */
int
p2m_pod_decrease_reservation(struct domain *d,
                             xen_pfn_t gpfn,
                             unsigned int order)
{
    int ret=0;
    int i;
    struct p2m_domain *p2m = p2m_get_hostp2m(d);

    int steal_for_cache = 0;
    int pod = 0, nonpod = 0, ram = 0;
    

    /* If we don't have any outstanding PoD entries, let things take their
     * course */
    if ( p2m->pod.entry_count == 0 )
        goto out;

    /* Figure out if we need to steal some freed memory for our cache */
    steal_for_cache =  ( p2m->pod.entry_count > p2m->pod.count );

    p2m_lock(p2m);
    audit_p2m(p2m, 1);

    if ( unlikely(d->is_dying) )
        goto out_unlock;

    /* See what's in here. */
    /* FIXME: Add contiguous; query for PSE entries? */
    for ( i=0; i<(1<<order); i++)
    {
        p2m_access_t a;
        p2m_type_t t;

        (void)p2m->get_entry(p2m, gpfn + i, &t, &a, p2m_query, NULL);

        if (p2m_is_pod(t))
            pod++;
        else
        {
            nonpod++;
            if ( p2m_is_ram(t) )
                ram++;
        }
    }

    /* No populate-on-demand?  Don't need to steal anything?  Then we're done!*/
    if(!pod && !steal_for_cache)
        goto out_unlock;

    if ( !nonpod )
    {
        /* All PoD: Mark the whole region invalid and tell caller
         * we're done. */
        set_p2m_entry(p2m, gpfn, _mfn(INVALID_MFN), order, p2m_invalid, p2m->default_access);
        p2m->pod.entry_count-=(1<<order); /* Lock: p2m */
        BUG_ON(p2m->pod.entry_count < 0);
        ret = 1;
        goto out_entry_check;
    }

    /* FIXME: Steal contig 2-meg regions for cache */

    /* Process as long as:
     * + There are PoD entries to handle, or
     * + There is ram left, and we want to steal it
     */
    for ( i=0;
          i<(1<<order) && (pod>0 || (steal_for_cache && ram > 0));
          i++)
    {
        mfn_t mfn;
        p2m_type_t t;
        p2m_access_t a;

        mfn = p2m->get_entry(p2m, gpfn + i, &t, &a, p2m_query, NULL);
        if (p2m_is_pod(t)) {
            set_p2m_entry(p2m, gpfn + i, _mfn(INVALID_MFN), 0, p2m_invalid, p2m->default_access);
            p2m->pod.entry_count--; /* Lock: p2m */
            BUG_ON(p2m->pod.entry_count < 0);
            pod--;
        }
        else if ( steal_for_cache && p2m_is_ram(t) )
        {
            struct page_info *page;

            ASSERT(mfn_valid(mfn));

            page = mfn_to_page(mfn);

            set_p2m_entry(p2m, gpfn + i, _mfn(INVALID_MFN), 0, p2m_invalid, p2m->default_access);
            set_gpfn_from_mfn(mfn_x(mfn), INVALID_M2P_ENTRY);

            p2m_pod_cache_add(p2m, page, 0);

            steal_for_cache =  ( p2m->pod.entry_count > p2m->pod.count );

            nonpod--;
            ram--;
        }
    }    

    /* If there are no more non-PoD entries, tell decrease_reservation() that
     * there's nothing left to do. */
    if ( nonpod == 0 )
        ret = 1;

out_entry_check:
    /* If we've reduced our "liabilities" beyond our "assets", free some */
    if ( p2m->pod.entry_count < p2m->pod.count )
    {
        p2m_pod_set_cache_target(p2m, p2m->pod.entry_count, 0/*can't preempt*/);
    }

out_unlock:
    audit_p2m(p2m, 1);
    p2m_unlock(p2m);

out:
    return ret;
}

void p2m_pod_dump_data(struct domain *d)
{
    struct p2m_domain *p2m = p2m_get_hostp2m(d);
    printk("    PoD entries=%d cachesize=%d\n",
           p2m->pod.entry_count, p2m->pod.count);
}


/* Search for all-zero superpages to be reclaimed as superpages for the
 * PoD cache. Must be called w/ p2m lock held, page_alloc lock not held. */
static int
p2m_pod_zero_check_superpage(struct p2m_domain *p2m, unsigned long gfn)
{
    mfn_t mfn, mfn0 = _mfn(INVALID_MFN);
    p2m_type_t type, type0 = 0;
    unsigned long * map = NULL;
    int ret=0, reset = 0;
    int i, j;
    int max_ref = 1;
    struct domain *d = p2m->domain;

    if ( !superpage_aligned(gfn) )
        goto out;

    /* Allow an extra refcount for one shadow pt mapping in shadowed domains */
    if ( paging_mode_shadow(d) )
        max_ref++;

    /* Look up the mfns, checking to make sure they're the same mfn
     * and aligned, and mapping them. */
    for ( i=0; i<SUPERPAGE_PAGES; i++ )
    {
        p2m_access_t a; 
        mfn = p2m->get_entry(p2m, gfn + i, &type, &a, p2m_query, NULL);

        if ( i == 0 )
        {
            mfn0 = mfn;
            type0 = type;
        }

        /* Conditions that must be met for superpage-superpage:
         * + All gfns are ram types
         * + All gfns have the same type
         * + All of the mfns are allocated to a domain
         * + None of the mfns are used as pagetables, or allocated via xenheap
         * + The first mfn is 2-meg aligned
         * + All the other mfns are in sequence
         * Adding for good measure:
         * + None of the mfns are likely to be mapped elsewhere (refcount
         *   2 or less for shadow, 1 for hap)
         */
        if ( !p2m_is_ram(type)
             || type != type0
             || ( (mfn_to_page(mfn)->count_info & PGC_allocated) == 0 )
             || ( (mfn_to_page(mfn)->count_info & (PGC_page_table|PGC_xen_heap)) != 0 )
             || ( (mfn_to_page(mfn)->count_info & PGC_xen_heap  ) != 0 )
             || ( (mfn_to_page(mfn)->count_info & PGC_count_mask) > max_ref )
             || !( ( i == 0 && superpage_aligned(mfn_x(mfn0)) )
                   || ( i != 0 && mfn_x(mfn) == (mfn_x(mfn0) + i) ) ) )
            goto out;
    }

    /* Now, do a quick check to see if it may be zero before unmapping. */
    for ( i=0; i<SUPERPAGE_PAGES; i++ )
    {
        /* Quick zero-check */
        map = map_domain_page(mfn_x(mfn0) + i);

        for ( j=0; j<16; j++ )
            if( *(map+j) != 0 )
                break;

        unmap_domain_page(map);

        if ( j < 16 )
            goto out;

    }

    /* Try to remove the page, restoring old mapping if it fails. */
    set_p2m_entry(p2m, gfn, _mfn(0), PAGE_ORDER_2M,
                  p2m_populate_on_demand, p2m->default_access);

    /* Make none of the MFNs are used elsewhere... for example, mapped
     * via the grant table interface, or by qemu.  Allow one refcount for
     * being allocated to the domain. */
    for ( i=0; i < SUPERPAGE_PAGES; i++ )
    {
        mfn = _mfn(mfn_x(mfn0) + i);
        if ( (mfn_to_page(mfn)->count_info & PGC_count_mask) > 1 )
        {
            reset = 1;
            goto out_reset;
        }
    }

    /* Finally, do a full zero-check */
    for ( i=0; i < SUPERPAGE_PAGES; i++ )
    {
        map = map_domain_page(mfn_x(mfn0) + i);

        for ( j=0; j<PAGE_SIZE/sizeof(*map); j++ )
            if( *(map+j) != 0 )
            {
                reset = 1;
                break;
            }

        unmap_domain_page(map);

        if ( reset )
            goto out_reset;
    }

    if ( tb_init_done )
    {
        struct {
            u64 gfn, mfn;
            int d:16,order:16;
        } t;

        t.gfn = gfn;
        t.mfn = mfn_x(mfn);
        t.d = d->domain_id;
        t.order = 9;

        __trace_var(TRC_MEM_POD_ZERO_RECLAIM, 0, sizeof(t), &t);
    }

    /* Finally!  We've passed all the checks, and can add the mfn superpage
     * back on the PoD cache, and account for the new p2m PoD entries */
    p2m_pod_cache_add(p2m, mfn_to_page(mfn0), PAGE_ORDER_2M);
    p2m->pod.entry_count += SUPERPAGE_PAGES;

out_reset:
    if ( reset )
        set_p2m_entry(p2m, gfn, mfn0, 9, type0, p2m->default_access);
    
out:
    return ret;
}

static void
p2m_pod_zero_check(struct p2m_domain *p2m, unsigned long *gfns, int count)
{
    mfn_t mfns[count];
    p2m_type_t types[count];
    unsigned long * map[count];
    struct domain *d = p2m->domain;

    int i, j;
    int max_ref = 1;

    /* Allow an extra refcount for one shadow pt mapping in shadowed domains */
    if ( paging_mode_shadow(d) )
        max_ref++;

    /* First, get the gfn list, translate to mfns, and map the pages. */
    for ( i=0; i<count; i++ )
    {
        p2m_access_t a;
        mfns[i] = p2m->get_entry(p2m, gfns[i], types + i, &a, p2m_query, NULL);
        /* If this is ram, and not a pagetable or from the xen heap, and probably not mapped
           elsewhere, map it; otherwise, skip. */
        if ( p2m_is_ram(types[i])
             && ( (mfn_to_page(mfns[i])->count_info & PGC_allocated) != 0 ) 
             && ( (mfn_to_page(mfns[i])->count_info & (PGC_page_table|PGC_xen_heap|PGC_mapcache)) == 0 ) 
             && ( (mfn_to_page(mfns[i])->count_info & PGC_count_mask) <= max_ref ) )
            map[i] = map_domain_page(mfn_x(mfns[i]));
        else
            map[i] = NULL;
    }

    /* Then, go through and check for zeroed pages, removing write permission
     * for those with zeroes. */
    for ( i=0; i<count; i++ )
    {
        if(!map[i])
            continue;

        /* Quick zero-check */
        for ( j=0; j<16; j++ )
            if( *(map[i]+j) != 0 )
                break;

        if ( j < 16 )
        {
            unmap_domain_page(map[i]);
            map[i] = NULL;
            continue;
        }

        /* Try to remove the page, restoring old mapping if it fails. */
        set_p2m_entry(p2m, gfns[i], shared_zero_page, PAGE_ORDER_4K,
                      p2m_populate_on_demand, p2m->default_access);

        /* See if the page was successfully unmapped.  (Allow one refcount
         * for being allocated to a domain.) */
        if ( (mfn_to_page(mfns[i])->count_info & PGC_count_mask) > 1 )
        {
            unmap_domain_page(map[i]);
            map[i] = NULL;

            set_p2m_entry(p2m, gfns[i], mfns[i], PAGE_ORDER_4K,
                types[i], p2m->default_access);

            continue;
        }
    }

    /* Now check each page for real */
    for ( i=0; i < count; i++ )
    {
        if(!map[i])
            continue;

        for ( j=0; j<PAGE_SIZE/sizeof(*map[i]); j++ )
            if( *(map[i]+j) != 0 )
                break;

        unmap_domain_page(map[i]);

        /* See comment in p2m_pod_zero_check_superpage() re gnttab
         * check timing.  */
        if ( j < PAGE_SIZE/sizeof(*map[i]) )
        {
            set_p2m_entry(p2m, gfns[i], mfns[i], PAGE_ORDER_4K,
                types[i], p2m->default_access);
        }
        else
        {
            if ( tb_init_done )
            {
                struct {
                    u64 gfn, mfn;
                    int d:16,order:16;
                } t;

                t.gfn = gfns[i];
                t.mfn = mfn_x(mfns[i]);
                t.d = d->domain_id;
                t.order = 0;
        
                __trace_var(TRC_MEM_POD_ZERO_RECLAIM, 0, sizeof(t), &t);
            }

            /* Add to cache, and account for the new p2m PoD entry */
            p2m_pod_cache_add(p2m, mfn_to_page(mfns[i]), PAGE_ORDER_4K);
            p2m->pod.entry_count++;
        }
    }
    
}

#define POD_SWEEP_LIMIT 1024
static void
p2m_pod_emergency_sweep_super(struct p2m_domain *p2m)
{
    unsigned long i, start, limit;

    if ( p2m->pod.reclaim_super == 0 )
    {
        p2m->pod.reclaim_super = (p2m->pod.max_guest>>PAGE_ORDER_2M)<<PAGE_ORDER_2M;
        p2m->pod.reclaim_super -= SUPERPAGE_PAGES;
    }
    
    start = p2m->pod.reclaim_super;
    limit = (start > POD_SWEEP_LIMIT) ? (start - POD_SWEEP_LIMIT) : 0;

    for ( i=p2m->pod.reclaim_super ; i > 0 ; i -= SUPERPAGE_PAGES )
    {
        p2m_pod_zero_check_superpage(p2m, i);
        /* Stop if we're past our limit and we have found *something*.
         *
         * NB that this is a zero-sum game; we're increasing our cache size
         * by increasing our 'debt'.  Since we hold the p2m lock,
         * (entry_count - count) must remain the same. */
        if ( !page_list_empty(&p2m->pod.super) &&  i < limit )
            break;
    }

    p2m->pod.reclaim_super = i ? i - SUPERPAGE_PAGES : 0;
}

#define POD_SWEEP_STRIDE  16
static void
p2m_pod_emergency_sweep(struct p2m_domain *p2m)
{
    unsigned long gfns[POD_SWEEP_STRIDE];
    unsigned long i, j=0, start, limit;
    p2m_type_t t;


    if ( p2m->pod.reclaim_single == 0 )
        p2m->pod.reclaim_single = p2m->pod.max_guest;

    start = p2m->pod.reclaim_single;
    limit = (start > POD_SWEEP_LIMIT) ? (start - POD_SWEEP_LIMIT) : 0;

    /* FIXME: Figure out how to avoid superpages */
    for ( i=p2m->pod.reclaim_single; i > 0 ; i-- )
    {
        p2m_access_t a;
        (void)p2m->get_entry(p2m, i, &t, &a, p2m_query, NULL);
        if ( p2m_is_ram(t) )
        {
            gfns[j] = i;
            j++;
            BUG_ON(j > POD_SWEEP_STRIDE);
            if ( j == POD_SWEEP_STRIDE )
            {
                p2m_pod_zero_check(p2m, gfns, j);
                j = 0;
            }
        }
        /* Stop if we're past our limit and we have found *something*.
         *
         * NB that this is a zero-sum game; we're increasing our cache size
         * by re-increasing our 'debt'.  Since we hold the p2m lock,
         * (entry_count - count) must remain the same. */
        if ( p2m->pod.count > 0 && i < limit )
            break;
    }

    if ( j )
        p2m_pod_zero_check(p2m, gfns, j);

    p2m->pod.reclaim_single = i ? i - 1 : i;

}
#endif  /* __UXEN__ */

static void check_immutable(p2m_query_t q, struct domain *d, unsigned long gfn)
{
    struct domain *template = d->clone_of;
    struct p2m_domain *p2m;
    p2m_type_t t;
    p2m_access_t a;
    mfn_t mfn;
    int err;

/* The "!is_p2m_guest_query(q)" check is a bit fragile. It is supposed to mean:
   I was called by  hvm_hap_nested_page_fault, via
   1295     mfn = get_gfn_type_access(p2m, gfn, &p2mt, &p2ma, p2m_guest, NULL);
   line. It is the only occurence of rvalue p2m_guest, though */
    if (!is_p2m_guest_query(q))
        return;
    if (!template || !(current->domain->introspection_features & XEN_DOMCTL_INTROSPECTION_FEATURE_IMMUTABLE_MEMORY))
        return;
    p2m = p2m_get_hostp2m(template);
    mfn = p2m->get_entry(p2m, gfn, &t, &a, p2m_query, NULL);
    if (mfn_x(mfn) == INVALID_MFN)
       gdprintk(XENLOG_WARNING, "INVALID_MFN for gfn 0x%lx in the template?\n", gfn);
    else if (p2m_is_immutable(t)) {
        gdprintk(XENLOG_WARNING, "write to immutable gfn 0x%lx\n", gfn);
        vmcs_mini_dump_vcpu(current, 0xaabbccdd);
        send_introspection_ioreq_detailed(
            XEN_DOMCTL_INTROSPECTION_FEATURE_IMMUTABLE_MEMORY,
            guest_cpu_user_regs()->eip,
            __vmread_safe(GUEST_LINEAR_ADDRESS, &err));
    }
}

int
p2m_pod_demand_populate(struct p2m_domain *p2m, unsigned long gfn,
                        unsigned int order, p2m_query_t q, void *entry)
{
    struct domain *d = p2m->domain;
    struct page_info *p = NULL; /* Compiler warnings */
    unsigned long gfn_aligned;
    mfn_t mfn, smfn;
    p2m_type_t t;
    p2m_access_t a;
    void *source, *target;
    int smfn_from_clone = 1;
    int ret;

    /* This is called from the p2m lookups, which can happen with or 
     * without the lock hed. */
    p2m_lock_recursive(p2m);

    /* This check is done with the p2m lock held.  This will make sure that
     * even if d->is_dying changes under our feet, p2m_pod_empty_cache() 
     * won't start until we're done. */
    if ( unlikely(d->is_dying) )
        goto out_fail;

    gfn_aligned = (gfn >> order) << order;

    /* parse entry with lock held */
    smfn = p2m->parse_entry(entry, 0, &t, &a);

    /* Check to make sure this is still PoD, also check for spurious
     * read accesses to entries already populated from other vcpus. */
    if (!p2m_is_pod(t) || ((q == p2m_guest_r || q == p2m_alloc_r) &&
                           mfn_valid_page(mfn_x(smfn)))) {
        p2m_unlock(p2m);
        return 0;
    }

    switch (order) {
    case PAGE_ORDER_1G:
        /* Because PoD does not have cache list for 1GB pages, it has to remap
         * 1GB region to 2MB chunks for a retry. */
        /* Note that we are supposed to call set_p2m_entry() 512 times to 
         * split 1GB into 512 2MB pages here. But We only do once here because
         * set_p2m_entry() should automatically shatter the 1GB page into 
         * 512 2MB pages. The rest of 511 calls are unnecessary.
         */
        set_p2m_entry(p2m, gfn_aligned, _mfn(0), PAGE_ORDER_2M,
                      p2m_populate_on_demand, p2m->default_access);
        audit_p2m(p2m, 1);
        p2m_unlock(p2m);
        return 0;
    case PAGE_ORDER_2M:
        if (!d->clone_of) {
            gdprintk(XENLOG_ERR, "PAGE_ORDER_2M pod in non-clone VM\n");
            domain_crash(d);
            goto out_fail;
        }

        ret = p2m_clone_l1(p2m_get_hostp2m(d->clone_of), p2m, gfn_aligned,
                           entry);
        p2m_unlock(p2m);
        return ret;
    }

#ifndef __UXEN__
    /* Once we've ballooned down enough that we can fill the remaining
     * PoD entries from the cache, don't sweep even if the particular
     * list we want to use is empty: that can lead to thrashing zero pages 
     * through the cache for no good reason.  */
    if ( p2m->pod.entry_count > p2m->pod.count )
    {

        /* If we're low, start a sweep */
        if ( order == PAGE_ORDER_2M && page_list_empty(&p2m->pod.super) )
            p2m_pod_emergency_sweep_super(p2m);

        if ( page_list_empty(&p2m->pod.single) &&
             ( ( order == PAGE_ORDER_4K )
               || (order == PAGE_ORDER_2M && page_list_empty(&p2m->pod.super) ) ) )
            p2m_pod_emergency_sweep(p2m);
    }
#endif  /* __UXEN__ */

    /* Keep track of the highest gfn demand-populated by a guest fault */
    if (is_p2m_guest_query(q) && gfn > p2m->pod.max_guest)
        p2m->pod.max_guest = gfn;

#ifndef __UXEN__
    lock_page_alloc(p2m);

    if ( p2m->pod.count == 0 )
        goto out_of_memory;

    /* Get a page f/ the cache.  A NULL return value indicates that the
     * 2-meg range should be marked singleton PoD, and retried */
    if ( (p = p2m_pod_cache_get(p2m, order)) == NULL )
        goto out_of_memory;

    mfn = page_to_mfn(p);

    BUG_ON((mfn_x(mfn) & ((1 << order)-1)) != 0);

    unlock_page_alloc(p2m);
#endif  /* __UXEN__ */

    ASSERT(order == 0);
    ASSERT(mfn_x(smfn) != INVALID_MFN);

    if (is_p2m_zeroshare_any(q)) {
        if (mfn_x(smfn) != mfn_x(shared_zero_page)) {
            /* not already zero shared */
            set_p2m_entry(p2m, gfn, shared_zero_page, PAGE_ORDER_4K,
                          p2m_populate_on_demand, p2m->default_access);
            atomic_inc(&d->zero_shared_pages);
            /* replacing non-pod page? */
            if (!p2m_is_pod(t))
                atomic_inc(&d->pod_pages);
            /* replacing a template shared page? */
            else if (mfn_valid_page(mfn_x(smfn)))
                atomic_dec(&d->tmpl_shared_pages);
        }
        audit_p2m(p2m, 1);
        p2m_unlock(p2m);
        return 0;
    }

    if (mfn_x(smfn) == 0) {
        struct p2m_domain *op2m = p2m_get_hostp2m(d->clone_of);
        smfn = op2m->get_entry(op2m, gfn_aligned, &t, &a, p2m_query, NULL);
        if (mfn_x(smfn) == INVALID_MFN) {
            /* clear this ept entry since it's not present in the
             * template p2m -- this happens if the l1 is accessed/used
             * between when it's allocated and filled with pod entries
             * (xpt_split_super_page), and when its entries are copied
             * from the template (clone_l1_table), or more often in
             * the case where the l1 table is populated lazily
             * (HVM_PARAM_CLONE_L1_lazy_populate) */
            set_p2m_entry(p2m, gfn_aligned, _mfn(0), 0, 0, 0);
            atomic_dec(&d->pod_pages);
            p2m_unlock(p2m);
            return 1;
        }
        if ((q == p2m_guest_r || q == p2m_alloc_r) &&
            mfn_valid_page(mfn_x(smfn))) {
            /* read-acces -- add pod entry, i.e. make the gpfn shared */
            set_p2m_entry(p2m, gfn_aligned, smfn, 0,
                          p2m_populate_on_demand, p2m->default_access);
            if (mfn_x(smfn) == mfn_x(shared_zero_page))
                atomic_inc(&d->zero_shared_pages);
            else
                atomic_inc(&d->tmpl_shared_pages);
            p2m_unlock(p2m);
            return 0;
        }
        smfn_from_clone = 0;
    }

#ifdef __UXEN__
    p = alloc_domheap_page(d, PAGE_ORDER_4K);
    if (!p)
        goto out_of_memory;
    mfn = page_to_mfn(p);
#endif  /* __UXEN__ */

    target = map_domain_page_direct(mfn_x(mfn));
    if (mfn_x(smfn) != mfn_x(shared_zero_page)) {
        source = map_domain_page(mfn_x(smfn));
        memcpy(target, source, PAGE_SIZE);
        unmap_domain_page(source);
	perfc_incr(pc15);
        if (smfn_from_clone)
            atomic_dec(&d->tmpl_shared_pages);
	check_immutable(q, d, gfn_aligned);
    } else {
        clear_page(target);
	perfc_incr(pc14);
        if (smfn_from_clone)
            atomic_dec(&d->zero_shared_pages);
    }
    unmap_domain_page_direct(target);

    set_p2m_entry(p2m, gfn_aligned, mfn, PAGE_ORDER_4K, p2m_ram_rw,
                  p2m->default_access);
    atomic_dec(&d->pod_pages);

    set_gpfn_from_mfn(mfn_x(mfn), gfn_aligned);
    paging_mark_dirty(d, mfn_x(mfn));
    
#ifndef __UXEN__
    p2m->pod.entry_count -= (1 << order); /* Lock: p2m */
    BUG_ON(p2m->pod.entry_count < 0);
#endif  /* __UXEN__ */

    if ( tb_init_done )
    {
        struct {
            u64 gfn, mfn;
            int d:16,order:16;
        } t;

        t.gfn = gfn;
        t.mfn = mfn_x(mfn);
        t.d = d->domain_id;
        t.order = order;
        
        __trace_var(TRC_MEM_POD_POPULATE, 0, sizeof(t), &t);
    }

    p2m_unlock(p2m);
    return 0;
out_of_memory:
#ifndef __UXEN__
    unlock_page_alloc(p2m);
#endif  /* __UXEN__ */

    printk("%s: Out of populate-on-demand memory! tot_pages %" PRIu32 " pod_pages %" PRIi32 "\n",
        __func__, d->tot_pages, atomic_read(&d->pod_pages));
    domain_crash(d);
out_fail:
    p2m_unlock(p2m);
    return -1;
#ifndef __UXEN__
remap_and_retry:
    DEBUG();
    BUG_ON(order != PAGE_ORDER_2M);
    unlock_page_alloc(p2m);

    /* Remap this 2-meg region in singleton chunks */
    gfn_aligned = (gfn>>order)<<order;
    for(i=0; i<(1<<order); i++)
        set_p2m_entry(p2m, gfn_aligned+i, shared_zero_page,
		      PAGE_ORDER_4K,
                      p2m_populate_on_demand, p2m->default_access);
    if ( tb_init_done )
    {
        struct {
            u64 gfn;
            int d:16;
        } t;

        t.gfn = gfn;
        t.d = d->domain_id;
        
        __trace_var(TRC_MEM_POD_SUPERPAGE_SPLINTER, 0, sizeof(t), &t);
    }

    p2m_unlock(p2m);
    return 0;
#endif  /* __UXEN__ */
}

static int
clone_l1_table(struct p2m_domain *op2m, struct p2m_domain *p2m,
               unsigned long *_gpfn, void *otable, void *table)
{
    struct domain *d = p2m->domain;
    unsigned long gpfn = *_gpfn;
    unsigned long index = gpfn & ((1UL << PAGETABLE_ORDER) - 1);
    mfn_t mfn;
    p2m_type_t t;
    p2m_access_t a;
    int ret = 0;

    if (d->arch.hvm_domain.params[HVM_PARAM_CLONE_L1] && !table)
        atomic_sub(L1_PAGETABLE_ENTRIES - index, &d->pod_pages);
    while (index < L1_PAGETABLE_ENTRIES) {
        mfn = op2m->parse_entry(otable, index, &t, &a);
        if (table && (p2m_is_pod(t) || p2m_is_ram(t))) {
            p2m_type_t _t;
            p2m_access_t _a;
            p2m->parse_entry(table, index, &_t, &_a);
            if (!p2m_is_pod(_t))
                atomic_inc(&d->pod_pages);
        }
        if (p2m_is_pod(t)) {
            ret = !set_p2m_entry(p2m, gpfn, shared_zero_page, 0,
                                 p2m_populate_on_demand, p2m->default_access);
            if (ret) {
                gdprintk(XENLOG_ERR, "%s: set_p2m_entry failed gpfn=%08lx\n",
                         __FUNCTION__, gpfn);
                goto out;
            }
            if (!table)
                atomic_inc(&d->pod_pages);
            atomic_inc(&d->zero_shared_pages);
        } else if (p2m_is_ram(t)) {
            ret = !set_p2m_entry(p2m, gpfn, mfn, 0,
                                 p2m_populate_on_demand, p2m->default_access);
            if (ret) {
                gdprintk(XENLOG_ERR, "%s: set_p2m_entry failed gpfn=%08lx\n",
                         __FUNCTION__, gpfn);
                goto out;
            }
            if (!table)
                atomic_inc(&d->pod_pages);
            atomic_inc(&d->tmpl_shared_pages);
        }
        index++;
        gpfn++;
    }

  out:
    *_gpfn = gpfn;
    return ret;
}

static int
p2m_clone_l1(struct p2m_domain *op2m, struct p2m_domain *p2m,
             unsigned long gpfn, void *entry)
{
    void *otable = NULL, *table = NULL;
    mfn_t mfn;
    p2m_type_t t;
    p2m_access_t a;
    int ret = 0;

    ASSERT(p2m_locked_by_me(p2m));

    if ((p2m->domain->arch.hvm_domain.params[HVM_PARAM_CLONE_L1] &
         HVM_PARAM_CLONE_L1_lazy_populate) && p2m->split_super_page_one &&
        !p2m->split_super_page_one(p2m, entry, PAGE_ORDER_2M))
        return 0;

    mfn = op2m->get_l1_table(op2m, gpfn, NULL);
    if (!mfn_valid_page(mfn_x(mfn)))
        return ret;
    otable = map_domain_page(mfn_x(mfn));

    mfn = p2m->parse_entry(entry, 0, &t, &a);
    if (mfn_valid_page(mfn_x(mfn)))
        table = map_domain_page(mfn_x(mfn));

    ret = clone_l1_table(op2m, p2m, &gpfn, otable, table);

    if (table)
        unmap_domain_page(table);
    unmap_domain_page(otable);
    return ret;
}

int
p2m_clone(struct p2m_domain *p2m, struct domain *nd)
{
    struct p2m_domain *np2m = p2m_get_hostp2m(nd);
    struct domain *d = p2m->domain;
    unsigned long gpfn;
    mfn_t mfn = _mfn(0);        /* compiler */
    mfn_t nmfn = _mfn(0);
    unsigned int page_order;
    void *table = NULL, *ntable = NULL;
    int ret = 0;
    s64 ct;

    p2m_lock(np2m);
    ct = -NOW();
    for (gpfn = 0; !ret && gpfn <= p2m->max_mapped_pfn; ) {
        if (!(gpfn & ((1UL << PAGETABLE_ORDER) - 1))) {
            mfn = p2m->get_l1_table(p2m, gpfn, &page_order);
            if (!mfn_valid_page(mfn_x(mfn))) {
                gpfn |= ((1 << page_order) - 1);
                gpfn++;
                continue;
            }
            nmfn = np2m->get_l1_table(np2m, gpfn, NULL);
        }
        if (hvm_hap_has_2mb(d) &&
            d->arch.hvm_domain.params[HVM_PARAM_CLONE_L1]) {
            /* if l1 exists already in clone, clone the rest of the l1
             * immediately */
            if (mfn_valid_page(mfn_x(nmfn)))
                goto clone_now;
            ret = !set_p2m_entry(np2m, gpfn, _mfn(0), PAGE_ORDER_2M,
                                 p2m_populate_on_demand, np2m->default_access);
            if (ret) {
                gdprintk(XENLOG_ERR, "%s: set_p2m_entry "
                         "shared l1 failed gpfn=%08lx\n",
                         __FUNCTION__, gpfn);
                continue;
            }
            gpfn += (1 << PAGE_ORDER_2M);
            atomic_inc(&nd->clone.l1_pod_pages);
            continue;
        }
      clone_now:
        if (!(gpfn & ((1UL << PAGETABLE_ORDER) - 1))) {
            if (ntable) {
                unmap_domain_page(ntable);
                ntable = NULL;
            }
            if (table)
                unmap_domain_page(table);
            table = map_domain_page(mfn_x(mfn));
            if (mfn_valid_page(mfn_x(nmfn)))
                ntable = map_domain_page(mfn_x(nmfn));
        }
        ret = clone_l1_table(p2m, np2m, &gpfn, table, ntable);
    }
    if (ntable)
        unmap_domain_page(ntable);
    if (table)
        unmap_domain_page(table);
    ct += NOW();
    p2m_unlock(np2m);

    printk("%s: domain %d took %"PRIu64".%"PRIu64"ms\n",
           __FUNCTION__, nd->domain_id, ct / 1000000UL, ct % 1000000UL);
    printk("domain %d: pod_pages=%d zero_shared=%d tmpl_shared=%d\n",
           nd->domain_id, atomic_read(&nd->pod_pages),
           atomic_read(&nd->zero_shared_pages),
           atomic_read(&nd->tmpl_shared_pages));
    if (atomic_read(&nd->clone.l1_pod_pages))
        printk("domain %d: l1_pod_pages=%d\n",
               nd->domain_id, atomic_read(&nd->clone.l1_pod_pages));
    return ret;
}

int
p2m_clear_gpfn_from_mapcache(struct p2m_domain *p2m, unsigned long gfn,
                             mfn_t mfn)
{
    struct domain *d = p2m->domain;
    struct page_info *page = mfn_to_page(mfn);
    int ret;

    spin_lock(&d->page_alloc_lock);
    ret = mdm_clear(d, gfn, 0);
    switch (ret) {
    case 1:
        perfc_incr(pc17);
        spin_unlock(&d->page_alloc_lock);
        return 1;
    case -1:
        if (!test_and_clear_bit(_PGC_mapcache, &page->count_info)) {
            gdprintk(XENLOG_INFO,
                     "Bad mapcache clear for page %lx in domain %u\n",
                     gfn, d->domain_id);
            break;
        }
        page_list_del(page, &d->mapcache_page_list);
        page_list_add_tail(page, is_xen_page(page) ?
                           &d->xenpage_list : &d->page_list);
        put_page(page);
        break;
    default:
        break;
    }
    spin_unlock(&d->page_alloc_lock);

    return 0;
}

int
p2m_pod_zero_share(struct p2m_domain *p2m, unsigned long gfn,
                   unsigned int order, p2m_query_t q, void *entry)
{
    struct domain *d = p2m->domain;
    mfn_t smfn;
    p2m_type_t p2mt;
    p2m_access_t p2ma;
    int ret = -1;

    /* This is called from the p2m lookups, which can happen with or 
     * without the lock hed. */
    p2m_lock_recursive(p2m);

    /* This check is done with the p2m lock held.  This will make sure that
     * even if d->is_dying changes under our feet, p2m_pod_empty_cache() 
     * won't start until we're done. */
    if (unlikely(d->is_dying))
        goto out;

    ASSERT(order == PAGE_ORDER_4K);

    ASSERT(mfn_x(smfn) != INVALID_MFN);

    /* parse entry with lock held */
    smfn = p2m->parse_entry(entry, 0, &p2mt, &p2ma);
    if (mfn_x(smfn) == mfn_x(shared_zero_page)) {
        ret = 0;
        goto out;
    }

    ret = p2m_clear_gpfn_from_mapcache(p2m, gfn, smfn);
    if (ret)
        goto out;

    set_p2m_entry(p2m, gfn, shared_zero_page, order,
		  p2m_populate_on_demand, p2m->default_access);

    /* Add to cache, and account for the new p2m PoD entry */
#ifndef __UXEN__
    p2m_pod_cache_add_zero(p2m, mfn_to_page(smfn), order);
#else  /* __UXEN__ */
    lock_page_alloc(p2m);
    page_list_del(mfn_to_page(smfn), &d->page_list);
    page_list_add_tail(mfn_to_page(smfn), &d->pod_free_list);
    unlock_page_alloc(p2m);
#endif  /* __UXEN__ */
    if (!p2m_is_pod(p2mt))
        atomic_inc(&d->pod_pages);
    else if (mfn_valid_page(mfn_x(smfn)))
        atomic_dec(&d->tmpl_shared_pages);
    atomic_inc(&d->zero_shared_pages);

    if ( tb_init_done )
    {
        struct {
            u64 gfn, mfn;
            int d:16,order:16;
        } t;

        t.gfn = gfn;
        t.mfn = mfn_x(smfn);
        t.d = d->domain_id;
        t.order = order;
        
        __trace_var(TRC_MEM_POD_ZERO_RECLAIM, 0, sizeof(t), &t);
    }

  out:
    p2m_unlock(p2m);
    return ret;
}


int
guest_physmap_mark_populate_on_demand(struct domain *d, unsigned long gfn,
                                      unsigned int order)
{
    struct p2m_domain *p2m = p2m_get_hostp2m(d);
    unsigned long i;
    p2m_type_t ot;
    mfn_t omfn;
    struct page_info *page = NULL;
    int pod_count = 0, pod_zero_count = 0, pod_tmpl_count = 0;
    int rc = 0;

    BUG_ON(!paging_mode_translate(d));

    rc = p2m_gfn_check_limit(d, gfn, order);
    if ( rc != 0 )
        return rc;

    p2m_lock(p2m);
    audit_p2m(p2m, 1);

    // P2M_DEBUG("mark pod gfn=%#lx\n", gfn);

    /* Make sure all gpfns are unused */
    for ( i = 0; i < (1UL << order); i++ )
    {
        p2m_access_t a;
        omfn = p2m->get_entry(p2m, gfn + i, &ot, &a, p2m_query, NULL);
        if ( p2m_is_ram(ot) )
        {
            ASSERT(mfn_valid(omfn));

            if (order) {
                dprintk(XENLOG_WARNING, "%s: dom %d: unsupported order != 0\n",
                    __func__, d->domain_id);
                rc = -EINVAL;
                goto out;
            }

            if (test_bit(_PGC_mapcache, &mfn_to_page(omfn)->count_info) &&
                p2m_clear_gpfn_from_mapcache(p2m, gfn + i, omfn))
                /* page has an active mapping in the mapcache --
                 * silently ignore and do nothing, which is arguably
                 * the equivalent of setting the gpfn to populate on
                 * demand, populating it with the current contents and
                 * then recreating the mapping in the mapcache */
                goto out;

            set_gpfn_from_mfn(mfn_x(omfn), INVALID_M2P_ENTRY);

            /* set page, to be freed after updating p2m entry */
            page = mfn_to_page(omfn);
        }
        else if (p2m_is_pod(ot)) {
            /* Count how many PoD entries we'll be replacing if successful */
            if (mfn_x(omfn) == 0)
                pod_count++;
            else if (mfn_x(omfn) == mfn_x(shared_zero_page))
                pod_zero_count++;
            else
                pod_tmpl_count++;
        }
    }

    /* Now, actually do the two-way mapping */
    if ( !set_p2m_entry(p2m, gfn, order ? _mfn(0) : shared_zero_page, order,
                        p2m_populate_on_demand, p2m->default_access) )
        rc = -EINVAL;
    else
    {
        if (page) {
            if (unlikely(!get_page(page, d)))
                dprintk(XENLOG_WARNING, "%s: dom %d: could not get page"
                        " mfn=%lx caf=%08lx owner=%d\n", __func__, d->domain_id,
                        __page_to_mfn(page), page->count_info,
                        page_get_owner(page) ? page_get_owner(page)->domain_id :
                        -1);
            else {
                if (test_and_clear_bit(_PGC_allocated, &page->count_info))
                    put_page(page);
                put_page(page);
            }
        }

#ifndef __UXEN__
        p2m->pod.entry_count += 1 << order; /* Lock: p2m */
        p2m->pod.entry_count -= (pod_count + pod_zero_count + pod_tmpl_count);
        BUG_ON(p2m->pod.entry_count < 0);
#else  /* __UXEN__ */
        atomic_add(1 << order, &d->pod_pages); /* Lock: p2m */
        atomic_sub(pod_count + pod_zero_count + pod_tmpl_count, &d->pod_pages);
        if (!order)
            atomic_add(1 << order, &d->zero_shared_pages);
        atomic_sub(pod_zero_count, &d->zero_shared_pages);
        atomic_sub(pod_tmpl_count, &d->tmpl_shared_pages);
#endif  /* __UXEN__ */
    }

out:
    audit_p2m(p2m, 1);
    p2m_unlock(p2m);

    return rc;
}

