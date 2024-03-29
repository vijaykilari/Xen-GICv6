#include <xen/config.h>
#include <xen/sched.h>
#include <xen/lib.h>
#include <xen/errno.h>
#include <xen/domain_page.h>
#include <xen/bitops.h>
#include <asm/flushtlb.h>
#include <asm/gic.h>
#include <asm/event.h>
#include <asm/hardirq.h>
#include <asm/page.h>

#ifdef CONFIG_ARM_64
/* Zeroeth level is of 1 page size */
#define P2M_ROOT_ORDER 0
#else
/* First level P2M is 2 consecutive pages */
#define P2M_ROOT_ORDER 1
#endif
#define P2M_FIRST_ENTRIES (LPAE_ENTRIES << P2M_ROOT_ORDER)

void dump_p2m_lookup(struct domain *d, paddr_t addr)
{
    struct p2m_domain *p2m = &d->arch.p2m;
    lpae_t *lookup;

    printk("dom%d IPA 0x%"PRIpaddr"\n", d->domain_id, addr);

#ifdef CONFIG_ARM_64
    if ( zeroeth_linear_offset(addr) > P2M_FIRST_ENTRIES )
    {
        printk("Beyond number of support entries at zeroeth level\n");
        return;
    }
#else
    if ( first_linear_offset(addr) > P2M_FIRST_ENTRIES )
    {
        printk("Cannot dump addresses in second of first level pages...\n");
        return;
    }
#endif

    printk("P2M @ %p mfn:0x%lx\n",
           p2m->root_level, page_to_mfn(p2m->root_level));

    lookup = __map_domain_page(p2m->root_level);
    dump_pt_walk(lookup, addr);
    unmap_domain_page(lookup);
}

static void p2m_load_VTTBR(struct domain *d)
{
    if ( is_idle_domain(d) )
        return;
    BUG_ON(!d->arch.vttbr);
    WRITE_SYSREG64(d->arch.vttbr, VTTBR_EL2);
    isb(); /* Ensure update is visible */
}

void p2m_save_state(struct vcpu *p)
{
    p->arch.sctlr = READ_SYSREG(SCTLR_EL1);
}

void p2m_restore_state(struct vcpu *n)
{
    register_t hcr;

    hcr = READ_SYSREG(HCR_EL2);
    WRITE_SYSREG(hcr & ~HCR_VM, HCR_EL2);
    isb();

    p2m_load_VTTBR(n->domain);
    isb();

    if ( is_32bit_domain(n->domain) )
        hcr &= ~HCR_RW;
    else
        hcr |= HCR_RW;

    WRITE_SYSREG(n->arch.sctlr, SCTLR_EL1);
    isb();

    WRITE_SYSREG(hcr, HCR_EL2);
    isb();
}

void flush_tlb_domain(struct domain *d)
{
    /* Update the VTTBR if necessary with the domain d. In this case,
     * it's only necessary to flush TLBs on every CPUs with the current VMID
     * (our domain).
     */
    if ( d != current->domain )
        p2m_load_VTTBR(d);

    flush_tlb();

    if ( d != current->domain )
        p2m_load_VTTBR(current->domain);
}

#ifdef CONFIG_ARM_64
/*
 * Map zeroeth level page that addr contains.
 */
static lpae_t *p2m_map_zeroeth(struct p2m_domain *p2m, paddr_t addr)
{
    if ( zeroeth_linear_offset(addr) >= LPAE_ENTRIES )
        return NULL;

    return __map_domain_page(p2m->root_level);
}

#else

static int p2m_first_level_index(paddr_t addr)
{
    /*
     * 1st pages are concatenated so zeroeth offset gives us the
     * index of the 1st page
     */
    return zeroeth_table_offset(addr);
}

/*
 * Map whichever of the first pages contain addr. The caller should
 * then use first_table_offset as an index.
 */
static lpae_t *p2m_map_first(struct p2m_domain *p2m, paddr_t addr)
{
    struct page_info *page;

    if ( first_linear_offset(addr) >= P2M_FIRST_ENTRIES )
        return NULL;

    page = p2m->root_level + p2m_first_level_index(addr);

    return __map_domain_page(page);
}
#endif

/*
 * Lookup the MFN corresponding to a domain's PFN.
 *
 * There are no processor functions to do a stage 2 only lookup therefore we
 * do a a software walk.
 */
paddr_t p2m_lookup(struct domain *d, paddr_t paddr, p2m_type_t *t)
{
    struct p2m_domain *p2m = &d->arch.p2m;
    lpae_t pte, *first = NULL, *second = NULL, *third = NULL;
#ifdef CONFIG_ARM_64
    lpae_t *zeroeth = NULL;
#endif
    paddr_t maddr = INVALID_PADDR;
    p2m_type_t _t;

    /* Allow t to be NULL */
    t = t ?: &_t;

    *t = p2m_invalid;

    spin_lock(&p2m->lock);

#ifdef CONFIG_ARM_64
    zeroeth = p2m_map_zeroeth(p2m, paddr);
    if ( !zeroeth )
        goto err;

    pte = zeroeth[zeroeth_table_offset(paddr)];
    /* Zeroeth level does not support block translation
     * so pte.p2m.table should be always set.
     * Just check for valid bit
     */
    if ( !pte.p2m.valid )
        goto done;

    /* Map first level table */
    first = map_domain_page(pte.p2m.base);
#else
    first = p2m_map_first(p2m, paddr);
    if ( !first )
        goto err;
#endif

    pte = first[first_table_offset(paddr)];
    if ( !pte.p2m.valid || !pte.p2m.table )
        goto done;

    second = map_domain_page(pte.p2m.base);
    pte = second[second_table_offset(paddr)];
    if ( !pte.p2m.valid || !pte.p2m.table )
        goto done;

    third = map_domain_page(pte.p2m.base);
    pte = third[third_table_offset(paddr)];

    /* This bit must be one in the level 3 entry */
    if ( !pte.p2m.table )
        pte.bits = 0;

done:
    if ( pte.p2m.valid )
    {
        ASSERT(pte.p2m.type != p2m_invalid);
        maddr = (pte.bits & PADDR_MASK & PAGE_MASK) | (paddr & ~PAGE_MASK);
        *t = pte.p2m.type;
    }

    if (third) unmap_domain_page(third);
    if (second) unmap_domain_page(second);
    if (first) unmap_domain_page(first);
#ifdef CONFIG_ARM_64
    if (zeroeth) unmap_domain_page(zeroeth);
#endif

err:
    spin_unlock(&p2m->lock);

    return maddr;
}

int guest_physmap_mark_populate_on_demand(struct domain *d,
                                          unsigned long gfn,
                                          unsigned int order)
{
    return -ENOSYS;
}

int p2m_pod_decrease_reservation(struct domain *d,
                                 xen_pfn_t gpfn,
                                 unsigned int order)
{
    return -ENOSYS;
}

static lpae_t mfn_to_p2m_entry(unsigned long mfn, unsigned int mattr,
                               p2m_type_t t)
{
    paddr_t pa = ((paddr_t) mfn) << PAGE_SHIFT;
    /* sh, xn and write bit will be defined in the following switches
     * based on mattr and t. */
    lpae_t e = (lpae_t) {
        .p2m.af = 1,
        .p2m.read = 1,
        .p2m.mattr = mattr,
        .p2m.table = 1,
        .p2m.valid = 1,
        .p2m.type = t,
    };

    BUILD_BUG_ON(p2m_max_real_type > (1 << 4));

    switch (mattr)
    {
    case MATTR_MEM:
        e.p2m.sh = LPAE_SH_INNER;
        break;

    case MATTR_DEV:
        e.p2m.sh = LPAE_SH_OUTER;
        break;
    default:
        BUG();
        break;
    }

    switch (t)
    {
    case p2m_ram_rw:
        e.p2m.xn = 0;
        e.p2m.write = 1;
        break;

    case p2m_ram_ro:
        e.p2m.xn = 0;
        e.p2m.write = 0;
        break;

    case p2m_iommu_map_rw:
    case p2m_map_foreign:
    case p2m_grant_map_rw:
    case p2m_mmio_direct:
        e.p2m.xn = 1;
        e.p2m.write = 1;
        break;

    case p2m_iommu_map_ro:
    case p2m_grant_map_ro:
    case p2m_invalid:
        e.p2m.xn = 1;
        e.p2m.write = 0;
        break;

    case p2m_max_real_type:
        BUG();
        break;
    }

    ASSERT(!(pa & ~PAGE_MASK));
    ASSERT(!(pa & ~PADDR_MASK));

    e.bits |= pa;

    return e;
}

static inline void p2m_write_pte(lpae_t *p, lpae_t pte, bool_t flush_cache)
{
    write_pte(p, pte);
    if ( flush_cache )
        clean_xen_dcache(*p);
}

/* Allocate a new page table page and hook it in via the given entry */
static int p2m_create_table(struct domain *d, lpae_t *entry, bool_t flush_cache)
{
    struct p2m_domain *p2m = &d->arch.p2m;
    struct page_info *page;
    void *p;
    lpae_t pte;

    BUG_ON(entry->p2m.valid);

    page = alloc_domheap_page(NULL, 0);
    if ( page == NULL )
        return -ENOMEM;

    page_list_add(page, &p2m->pages);

    p = __map_domain_page(page);
    clear_page(p);
    if ( flush_cache )
        clean_xen_dcache_va_range(p, PAGE_SIZE);
    unmap_domain_page(p);

    pte = mfn_to_p2m_entry(page_to_mfn(page), MATTR_MEM, p2m_invalid);

    p2m_write_pte(entry, pte, flush_cache);

    return 0;
}

enum p2m_operation {
    INSERT,
    ALLOCATE,
    REMOVE,
    RELINQUISH,
    CACHEFLUSH,
};

static int apply_p2m_changes(struct domain *d,
                     enum p2m_operation op,
                     paddr_t start_gpaddr,
                     paddr_t end_gpaddr,
                     paddr_t maddr,
                     int mattr,
                     p2m_type_t t)
{
    int rc;
    struct p2m_domain *p2m = &d->arch.p2m;
    lpae_t *first = NULL, *second = NULL, *third = NULL;
    paddr_t addr;
#ifdef CONFIG_ARM_64
    lpae_t *zeroeth = NULL;
    unsigned long cur_zeroeth_page = ~0,
                  cur_zeroeth_offset = ~0;
#else
    unsigned long cur_first_page = ~0;
#endif
    unsigned long cur_first_offset = ~0,
                  cur_second_offset = ~0;
    unsigned long count = 0;
    unsigned int flush = 0;
    bool_t populate = (op == INSERT || op == ALLOCATE);
    lpae_t pte;
    bool_t flush_pt;

    /* Some IOMMU don't support coherent PT walk. When the p2m is
     * shared with the CPU, Xen has to make sure that the PT changes have
     * reached the memory
     */
    flush_pt = iommu_enabled && !iommu_has_feature(d, IOMMU_FEAT_COHERENT_WALK);

    spin_lock(&p2m->lock);

    addr = start_gpaddr;
    while ( addr < end_gpaddr )
    {
#ifdef CONFIG_ARM_64
        /* Find zeroeth offset and map zeroeth page */
        if ( cur_zeroeth_page != zeroeth_table_offset(addr) )
        {
            if ( zeroeth ) unmap_domain_page(zeroeth);
            zeroeth = p2m_map_zeroeth(p2m, addr);
            if ( !zeroeth )
            {
                rc = -EINVAL;
                goto out;
            }
            cur_zeroeth_page = zeroeth_table_offset(addr);
        }

        if ( !zeroeth[zeroeth_table_offset(addr)].p2m.valid )
        {
            if ( !populate )
            {
                addr = (addr + ZEROETH_SIZE) & ZEROETH_MASK;
                continue;
            }
            rc = p2m_create_table(d, &zeroeth[zeroeth_table_offset(addr)]);
            if ( rc < 0 )
            {
                printk("p2m_populate_ram: L0 failed\n");
                goto out;
            }
        } 

        BUG_ON(!zeroeth[zeroeth_table_offset(addr)].p2m.valid);

        if ( cur_zeroeth_offset != zeroeth_table_offset(addr) )
        {
            if ( first ) unmap_domain_page(first);
            first = map_domain_page(zeroeth[zeroeth_table_offset(addr)].p2m.base);
            cur_zeroeth_offset = zeroeth_table_offset(addr);
        }
#else
        if ( cur_first_page != p2m_first_level_index(addr) )
        {
            if ( first ) unmap_domain_page(first);
            first = p2m_map_first(p2m, addr);
            if ( !first )
            {
                rc = -EINVAL;
                goto out;
            }
            cur_first_page = p2m_first_level_index(addr);
        }
#endif
        if ( !first[first_table_offset(addr)].p2m.valid )
        {
            if ( !populate )
            {
                addr = (addr + FIRST_SIZE) & FIRST_MASK;
                continue;
            }

            rc = p2m_create_table(d, &first[first_table_offset(addr)],
                                  flush_pt);
            if ( rc < 0 )
            {
                printk("p2m_populate_ram: L1 failed\n");
                goto out;
            }
        }

        BUG_ON(!first[first_table_offset(addr)].p2m.valid);

        if ( cur_first_offset != first_table_offset(addr) )
        {
            if (second) unmap_domain_page(second);
            second = map_domain_page(first[first_table_offset(addr)].p2m.base);
            cur_first_offset = first_table_offset(addr);
        }
        /* else: second already valid */

        if ( !second[second_table_offset(addr)].p2m.valid )
        {
            if ( !populate )
            {
                addr = (addr + SECOND_SIZE) & SECOND_MASK;
                continue;
            }

            rc = p2m_create_table(d, &second[second_table_offset(addr)],
                                  flush_pt);
            if ( rc < 0 ) {
                printk("p2m_populate_ram: L2 failed\n");
                goto out;
            }
        }

        BUG_ON(!second[second_table_offset(addr)].p2m.valid);

        if ( cur_second_offset != second_table_offset(addr) )
        {
            /* map third level */
            if (third) unmap_domain_page(third);
            third = map_domain_page(second[second_table_offset(addr)].p2m.base);
            cur_second_offset = second_table_offset(addr);
        }

        pte = third[third_table_offset(addr)];

        flush |= pte.p2m.valid;

        /* TODO: Handle other p2m type
         *
         * It's safe to do the put_page here because page_alloc will
         * flush the TLBs if the page is reallocated before the end of
         * this loop.
         */
        if ( pte.p2m.valid && p2m_is_foreign(pte.p2m.type) )
        {
            unsigned long mfn = pte.p2m.base;

            ASSERT(mfn_valid(mfn));
            put_page(mfn_to_page(mfn));
        }

        switch (op) {
            case ALLOCATE:
                {
                    /* Allocate a new RAM page and attach */
                    struct page_info *page;

                    ASSERT(!pte.p2m.valid);
                    rc = -ENOMEM;
                    page = alloc_domheap_page(d, 0);
                    if ( page == NULL ) {
                        printk("p2m_populate_ram: failed to allocate page\n");
                        goto out;
                    }

                    pte = mfn_to_p2m_entry(page_to_mfn(page), mattr, t);

                    p2m_write_pte(&third[third_table_offset(addr)],
                                  pte, flush_pt);
                }
                break;
            case INSERT:
                {
                    pte = mfn_to_p2m_entry(maddr >> PAGE_SHIFT, mattr, t);
                    p2m_write_pte(&third[third_table_offset(addr)],
                                  pte, flush_pt);
                    maddr += PAGE_SIZE;
                }
                break;
            case RELINQUISH:
            case REMOVE:
                {
                    if ( !pte.p2m.valid )
                    {
                        count++;
                        break;
                    }

                    count += 0x10;

                    memset(&pte, 0x00, sizeof(pte));
                    p2m_write_pte(&third[third_table_offset(addr)],
                                  pte, flush_pt);
                    count++;
                }
                break;

            case CACHEFLUSH:
                {
                    if ( !pte.p2m.valid || !p2m_is_ram(pte.p2m.type) )
                        break;

                    flush_page_to_ram(pte.p2m.base);
                }
                break;
        }

        /* Preempt every 2MiB (mapped) or 32 MiB (unmapped) - arbitrary */
        if ( op == RELINQUISH && count >= 0x2000 )
        {
            if ( hypercall_preempt_check() )
            {
                p2m->lowest_mapped_gfn = addr >> PAGE_SHIFT;
                rc = -ERESTART;
                goto out;
            }
            count = 0;
        }

        /* Got the next page */
        addr += PAGE_SIZE;
    }

    if ( flush )
    {
        unsigned long sgfn = paddr_to_pfn(start_gpaddr);
        unsigned long egfn = paddr_to_pfn(end_gpaddr);

        flush_tlb_domain(d);
        iommu_iotlb_flush(d, sgfn, egfn - sgfn);
    }

    if ( op == ALLOCATE || op == INSERT )
    {
        unsigned long sgfn = paddr_to_pfn(start_gpaddr);
        unsigned long egfn = paddr_to_pfn(end_gpaddr);

        p2m->max_mapped_gfn = MAX(p2m->max_mapped_gfn, egfn);
        p2m->lowest_mapped_gfn = MIN(p2m->lowest_mapped_gfn, sgfn);
    }

    rc = 0;

out:
    if (third) unmap_domain_page(third);
    if (second) unmap_domain_page(second);
    if (first) unmap_domain_page(first);
#ifdef CONFIG_ARM_64
    if ( zeroeth ) unmap_domain_page(zeroeth);
#endif

    spin_unlock(&p2m->lock);

    return rc;
}

int p2m_populate_ram(struct domain *d,
                     paddr_t start,
                     paddr_t end)
{
    return apply_p2m_changes(d, ALLOCATE, start, end,
                             0, MATTR_MEM, p2m_ram_rw);
}

int map_mmio_regions(struct domain *d,
                     paddr_t start_gaddr,
                     paddr_t end_gaddr,
                     paddr_t maddr)
{
    return apply_p2m_changes(d, INSERT, start_gaddr, end_gaddr,
                             maddr, MATTR_DEV, p2m_mmio_direct);
}

int guest_physmap_add_entry(struct domain *d,
                            unsigned long gpfn,
                            unsigned long mfn,
                            unsigned long page_order,
                            p2m_type_t t)
{
    return apply_p2m_changes(d, INSERT,
                             pfn_to_paddr(gpfn),
                             pfn_to_paddr(gpfn + (1 << page_order)),
                             pfn_to_paddr(mfn), MATTR_MEM, t);
}

void guest_physmap_remove_page(struct domain *d,
                               unsigned long gpfn,
                               unsigned long mfn, unsigned int page_order)
{
    apply_p2m_changes(d, REMOVE,
                      pfn_to_paddr(gpfn),
                      pfn_to_paddr(gpfn + (1<<page_order)),
                      pfn_to_paddr(mfn), MATTR_MEM, p2m_invalid);
}

int p2m_alloc_table(struct domain *d)
{
    struct p2m_domain *p2m = &d->arch.p2m;
    struct page_info *page;

    page = alloc_domheap_pages(NULL, P2M_ROOT_ORDER, 0);
    if ( page == NULL )
        return -ENOMEM;

    spin_lock(&p2m->lock);

    /* Clear both first level pages */
    p = __map_domain_page(page);
    clear_page(p);
    unmap_domain_page(p);

#ifdef CONFIG_ARM_32
    p = __map_domain_page(page + 1);
    clear_page(p);
    unmap_domain_page(p);
#endif

    p2m->root_level = page;

    d->arch.vttbr = page_to_maddr(p2m->root_level)
        | ((uint64_t)p2m->vmid&0xff)<<48;

    /* Make sure that all TLBs corresponding to the new VMID are flushed
     * before using it
     */
    flush_tlb_domain(d);

    spin_unlock(&p2m->lock);

    return 0;
}

#define MAX_VMID 256
#define INVALID_VMID 0 /* VMID 0 is reserved */

static spinlock_t vmid_alloc_lock = SPIN_LOCK_UNLOCKED;

/* VTTBR_EL2 VMID field is 8 bits. Using a bitmap here limits us to
 * 256 concurrent domains. */
static DECLARE_BITMAP(vmid_mask, MAX_VMID);

void p2m_vmid_allocator_init(void)
{
    set_bit(INVALID_VMID, vmid_mask);
}

static int p2m_alloc_vmid(struct domain *d)
{
    struct p2m_domain *p2m = &d->arch.p2m;

    int rc, nr;

    spin_lock(&vmid_alloc_lock);

    nr = find_first_zero_bit(vmid_mask, MAX_VMID);

    ASSERT(nr != INVALID_VMID);

    if ( nr == MAX_VMID )
    {
        rc = -EBUSY;
        printk(XENLOG_ERR "p2m.c: dom%d: VMID pool exhausted\n", d->domain_id);
        goto out;
    }

    set_bit(nr, vmid_mask);

    p2m->vmid = nr;

    rc = 0;

out:
    spin_unlock(&vmid_alloc_lock);
    return rc;
}

static void p2m_free_vmid(struct domain *d)
{
    struct p2m_domain *p2m = &d->arch.p2m;
    spin_lock(&vmid_alloc_lock);
    if ( p2m->vmid != INVALID_VMID )
        clear_bit(p2m->vmid, vmid_mask);

    spin_unlock(&vmid_alloc_lock);
}

void p2m_teardown(struct domain *d)
{
    struct p2m_domain *p2m = &d->arch.p2m;
    struct page_info *pg;

    spin_lock(&p2m->lock);

    while ( (pg = page_list_remove_head(&p2m->pages)) )
        free_domheap_page(pg);

    free_domheap_pages(p2m->root_level, P2M_ROOT_ORDER);

    p2m->root_level = NULL;

    p2m_free_vmid(d);

    spin_unlock(&p2m->lock);
}

int p2m_init(struct domain *d)
{
    struct p2m_domain *p2m = &d->arch.p2m;
    int rc = 0;

    spin_lock_init(&p2m->lock);
    INIT_PAGE_LIST_HEAD(&p2m->pages);

    spin_lock(&p2m->lock);
    p2m->vmid = INVALID_VMID;

    rc = p2m_alloc_vmid(d);
    if ( rc != 0 )
        goto err;

    d->arch.vttbr = 0;

    p2m->root_level = NULL;

    p2m->max_mapped_gfn = 0;
    p2m->lowest_mapped_gfn = ULONG_MAX;

err:
    spin_unlock(&p2m->lock);

    return rc;
}

int relinquish_p2m_mapping(struct domain *d)
{
    struct p2m_domain *p2m = &d->arch.p2m;

    return apply_p2m_changes(d, RELINQUISH,
                              pfn_to_paddr(p2m->lowest_mapped_gfn),
                              pfn_to_paddr(p2m->max_mapped_gfn),
                              pfn_to_paddr(INVALID_MFN),
                              MATTR_MEM, p2m_invalid);
}

int p2m_cache_flush(struct domain *d, xen_pfn_t start_mfn, xen_pfn_t end_mfn)
{
    struct p2m_domain *p2m = &d->arch.p2m;

    start_mfn = MAX(start_mfn, p2m->lowest_mapped_gfn);
    end_mfn = MIN(end_mfn, p2m->max_mapped_gfn);

    return apply_p2m_changes(d, CACHEFLUSH,
                             pfn_to_paddr(start_mfn),
                             pfn_to_paddr(end_mfn),
                             pfn_to_paddr(INVALID_MFN),
                             MATTR_MEM, p2m_invalid);
}

unsigned long gmfn_to_mfn(struct domain *d, unsigned long gpfn)
{
    paddr_t p = p2m_lookup(d, pfn_to_paddr(gpfn), NULL);
    return p >> PAGE_SHIFT;
}

struct page_info *get_page_from_gva(struct domain *d, vaddr_t va,
                                    unsigned long flags)
{
    struct p2m_domain *p2m = &d->arch.p2m;
    struct page_info *page = NULL;
    paddr_t maddr;

    ASSERT(d == current->domain);

    spin_lock(&p2m->lock);

    if ( gvirt_to_maddr(va, &maddr, flags) )
        goto err;

    if ( !mfn_valid(maddr >> PAGE_SHIFT) )
        goto err;

    page = mfn_to_page(maddr >> PAGE_SHIFT);
    ASSERT(page);

    if ( unlikely(!get_page(page, d)) )
        page = NULL;

err:
    spin_unlock(&p2m->lock);
    return page;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
