/*
 * xen/arch/arm/gic.c
 *
 * ARM Generic Interrupt Controller support
 *
 * Tim Deegan <tim@xen.org>
 * Copyright (c) 2011 Citrix Systems.
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
 */

#include <xen/config.h>
#include <xen/lib.h>
#include <xen/init.h>
#include <xen/mm.h>
#include <xen/irq.h>
#include <xen/sched.h>
#include <xen/errno.h>
#include <xen/softirq.h>
#include <xen/list.h>
#include <xen/device_tree.h>
#include <asm/p2m.h>
#include <asm/domain.h>
#include <asm/platform.h>
#include <asm/device.h>
#include <asm/io.h>
#include <asm/gic.h>

static void gic_restore_pending_irqs(struct vcpu *v);

static DEFINE_PER_CPU(uint64_t, lr_mask);

#define lr_all_full() (this_cpu(lr_mask) == ((1 << gic_hw_ops->info->nr_lrs) - 1))

#undef GIC_DEBUG

static void gic_update_one_lr(struct vcpu *v, int i);

static const struct gic_hw_operations *gic_hw_ops;

void register_gic_ops(const struct gic_hw_operations *ops)
{
    gic_hw_ops = ops;
}

static void update_cpu_lr_mask(void)
{
    this_cpu(lr_mask) = 0ULL;
}

enum gic_version gic_hw_version(void)
{
   return gic_hw_ops->info->hw_version;
}

unsigned int gic_number_lines(void)
{
    return gic_hw_ops->info->nr_lines;
}

void gic_save_state(struct vcpu *v)
{
    ASSERT(!local_irq_is_enabled());

    if ( is_idle_vcpu(v) )
        return;

    /* No need for spinlocks here because interrupts are disabled around
     * this call and it only accesses struct vcpu fields that cannot be
     * accessed simultaneously by another pCPU.
     */
    v->arch.lr_mask = this_cpu(lr_mask);
    gic_hw_ops->save_state(v);
    isb();
}

void gic_restore_state(struct vcpu *v)
{
    ASSERT(!local_irq_is_enabled());

    if ( is_idle_vcpu(v) )
        return;

    this_cpu(lr_mask) = v->arch.lr_mask;
    gic_hw_ops->restore_state(v);

    isb();

    gic_restore_pending_irqs(v);
}

/*
 * needs to be called with a valid cpu_mask, ie each cpu in the mask has
 * already called gic_cpu_init
 * - desc.lock must be held
 * - arch.type must be valid (i.e != DT_IRQ_TYPE_INVALID)
 */
static void gic_set_irq_properties(struct irq_desc *desc,
                                   const cpumask_t *cpu_mask,
                                   unsigned int priority)
{
   gic_hw_ops->set_irq_properties(desc, cpu_mask, priority);
}

/* Program the GIC to route an interrupt to the host (i.e. Xen)
 * - needs to be called with desc.lock held
 */
void gic_route_irq_to_xen(struct irq_desc *desc, const cpumask_t *cpu_mask,
                          unsigned int priority)
{
    ASSERT(priority <= 0xff);     /* Only 8 bits of priority */
    ASSERT(desc->irq < gic_number_lines());/* Can't route interrupts that don't exist */
    ASSERT(desc->status & IRQ_DISABLED);
    ASSERT(spin_is_locked(&desc->lock));

    desc->handler = gic_hw_ops->gic_host_irq_type;

    gic_set_irq_properties(desc, cpu_mask, priority);
}

/* Program the GIC to route an interrupt to a guest
 *   - desc.lock must be held
 */
void gic_route_irq_to_guest(struct domain *d, struct irq_desc *desc,
                            const cpumask_t *cpu_mask, unsigned int priority)
{
    struct pending_irq *p;
    ASSERT(spin_is_locked(&desc->lock));

    desc->handler = gic_hw_ops->gic_guest_irq_type;
    desc->status |= IRQ_GUEST;

    gic_set_irq_properties(desc, cpumask_of(smp_processor_id()), GIC_PRI_IRQ);

    /* TODO: do not assume delivery to vcpu0 */
    p = irq_to_pending(d->vcpu[0], desc->irq);
    p->desc = desc;
}

int gic_irq_xlate(const u32 *intspec, unsigned int intsize,
                  unsigned int *out_hwirq,
                  unsigned int *out_type)
{
    if ( intsize < 3 )
        return -EINVAL;

    /* Get the interrupt number and add 16 to skip over SGIs */
    *out_hwirq = intspec[1] + 16;

    /* For SPIs, we need to add 16 more to get the GIC irq ID number */
    if ( !intspec[0] )
        *out_hwirq += 16;

    if ( out_type )
        *out_type = intspec[2] & DT_IRQ_TYPE_SENSE_MASK;

    return 0;
}

/* Set up the GIC */
void __init gic_init(void)
{
    int rc;
    struct dt_device_node *node;
    bool_t num_gics = 0;

    dt_for_each_device_node( dt_host, node )
    {
        if ( !dt_get_property(node, "interrupt-controller", NULL) )
            continue;

        if ( !dt_get_parent(node) )
            continue;

        rc = device_init(node, DEVICE_GIC, NULL);
        if ( !rc )
        {
            /* NOTE: Only one GIC is supported */
            num_gics = 1;
            break;
        }
    }
    if ( !num_gics )
        panic("Unable to find compatible GIC in the device tree");

    /* Update cpu lr mask for cpu0 */
    update_cpu_lr_mask();
}

void send_SGI_mask(const cpumask_t *cpumask, enum gic_sgi sgi)
{
    cpumask_t online_mask;

    ASSERT(sgi < 16); /* There are only 16 SGIs */

    cpumask_and(&online_mask, cpumask, &cpu_online_map);

    dsb(sy);

    gic_hw_ops->send_SGI(sgi, SGI_TARGET_LIST, &online_mask);
}

void send_SGI_one(unsigned int cpu, enum gic_sgi sgi)
{
    send_SGI_mask(cpumask_of(cpu), sgi);
}

void send_SGI_self(enum gic_sgi sgi)
{
    ASSERT(sgi < 16); /* There are only 16 SGIs */

    dsb(sy);

    gic_hw_ops->send_SGI(sgi, SGI_TARGET_SELF, cpumask_of(smp_processor_id()));
}

void send_SGI_allbutself(enum gic_sgi sgi)
{
   cpumask_t all_others_mask;
   ASSERT(sgi < 16); /* There are only 16 SGIs */

   cpumask_andnot(&all_others_mask, &cpu_possible_map, cpumask_of(smp_processor_id()));
   dsb(sy);

   gic_hw_ops->send_SGI(sgi, SGI_TARGET_OTHERS, &all_others_mask);
}

void smp_send_state_dump(unsigned int cpu)
{
    send_SGI_one(cpu, GIC_SGI_DUMP_STATE);
}

/* Set up the per-CPU parts of the GIC for a secondary CPU */
void __cpuinit gic_init_secondary_cpu(void)
{
    gic_hw_ops->secondary_init();
    /* Update lr mask for secondary cpus */
    update_cpu_lr_mask();
}

/* Shut down the per-CPU GIC interface */
void gic_disable_cpu(void)
{
    ASSERT(!local_irq_is_enabled());

    gic_hw_ops->disable_interface();
}

static inline void gic_set_lr(int lr, struct pending_irq *p,
                              unsigned int state)
{
    ASSERT(!local_irq_is_enabled());

    gic_hw_ops->update_lr(lr, p, state);

    set_bit(GIC_IRQ_GUEST_VISIBLE, &p->status);
    clear_bit(GIC_IRQ_GUEST_QUEUED, &p->status);
    p->lr = lr;
}

static inline void gic_add_to_lr_pending(struct vcpu *v, struct pending_irq *n)
{
    struct pending_irq *iter;

    ASSERT(spin_is_locked(&v->arch.vgic.lock));

    if ( !list_empty(&n->lr_queue) )
        return;

    list_for_each_entry ( iter, &v->arch.vgic.lr_pending, lr_queue )
    {
        if ( iter->priority > n->priority )
        {
            list_add_tail(&n->lr_queue, &iter->lr_queue);
            return;
        }
    }
    list_add_tail(&n->lr_queue, &v->arch.vgic.lr_pending);
}

void gic_remove_from_queues(struct vcpu *v, unsigned int virtual_irq)
{
    struct pending_irq *p = irq_to_pending(v, virtual_irq);
    unsigned long flags;

    spin_lock_irqsave(&v->arch.vgic.lock, flags);
    if ( !list_empty(&p->lr_queue) )
        list_del_init(&p->lr_queue);
    spin_unlock_irqrestore(&v->arch.vgic.lock, flags);
}

void gic_raise_inflight_irq(struct vcpu *v, unsigned int virtual_irq)
{
    struct pending_irq *n = irq_to_pending(v, virtual_irq);

    ASSERT(spin_is_locked(&v->arch.vgic.lock));

    if ( list_empty(&n->lr_queue) )
    {
        if ( v == current )
            gic_update_one_lr(v, n->lr);
    }
#ifdef GIC_DEBUG
    else
        gdprintk(XENLOG_DEBUG, "trying to inject irq=%u into d%dv%d, when it is still lr_pending\n",
                 virtual_irq, v->domain->domain_id, v->vcpu_id);
#endif
}

void gic_raise_guest_irq(struct vcpu *v, unsigned int virtual_irq,
        unsigned int priority)
{
    int i;
    unsigned int nr_lrs = gic_hw_ops->info->nr_lrs;

    ASSERT(spin_is_locked(&v->arch.vgic.lock));

    if ( v == current && list_empty(&v->arch.vgic.lr_pending) )
    {
        i = find_first_zero_bit(&this_cpu(lr_mask), nr_lrs);
        if (i < nr_lrs) {
            set_bit(i, &this_cpu(lr_mask));
            gic_set_lr(i, irq_to_pending(v, virtual_irq), GICH_LR_PENDING);
            return;
        }
    }

    gic_add_to_lr_pending(v, irq_to_pending(v, virtual_irq));
}

static void gic_update_one_lr(struct vcpu *v, int i)
{
    struct pending_irq *p;
    int irq;
    struct gic_lr lr_val;

    ASSERT(spin_is_locked(&v->arch.vgic.lock));
    ASSERT(!local_irq_is_enabled());

    gic_hw_ops->read_lr(i, &lr_val);
    irq = lr_val.virq;
    p = irq_to_pending(v, irq);
    if ( lr_val.state & GICH_LR_ACTIVE )
    {
        set_bit(GIC_IRQ_GUEST_ACTIVE, &p->status);
        if ( test_bit(GIC_IRQ_GUEST_ENABLED, &p->status) &&
             test_and_clear_bit(GIC_IRQ_GUEST_QUEUED, &p->status) )
        {
            if ( p->desc == NULL )
            {
                 lr_val.state |= GICH_LR_PENDING;
                 gic_hw_ops->write_lr(i, &lr_val);
            }
            else
                gdprintk(XENLOG_WARNING, "unable to inject hw irq=%d into d%dv%d: already active in LR%d\n",
                         irq, v->domain->domain_id, v->vcpu_id, i);
        }
    }
    else if ( lr_val.state & GICH_LR_PENDING )
    {
        int q __attribute__ ((unused)) = test_and_clear_bit(GIC_IRQ_GUEST_QUEUED, &p->status);
#ifdef GIC_DEBUG
        if ( q )
            gdprintk(XENLOG_DEBUG, "trying to inject irq=%d into d%dv%d, when it is already pending in LR%d\n",
                    irq, v->domain->domain_id, v->vcpu_id, i);
#endif
    }
    else
    {
        gic_hw_ops->clear_lr(i);
        clear_bit(i, &this_cpu(lr_mask));

        if ( p->desc != NULL )
            p->desc->status &= ~IRQ_INPROGRESS;
        clear_bit(GIC_IRQ_GUEST_VISIBLE, &p->status);
        clear_bit(GIC_IRQ_GUEST_ACTIVE, &p->status);
        p->lr = GIC_INVALID_LR;
        if ( test_bit(GIC_IRQ_GUEST_ENABLED, &p->status) &&
             test_bit(GIC_IRQ_GUEST_QUEUED, &p->status) )
            gic_raise_guest_irq(v, irq, p->priority);
        else
            list_del_init(&p->inflight);
    }
}

void gic_clear_lrs(struct vcpu *v)
{
    int i = 0;
    unsigned long flags;
    unsigned int nr_lrs = gic_hw_ops->info->nr_lrs;

    /* The idle domain has no LRs to be cleared. Since gic_restore_state
     * doesn't write any LR registers for the idle domain they could be
     * non-zero. */
    if ( is_idle_vcpu(v) )
        return;

    spin_lock_irqsave(&v->arch.vgic.lock, flags);

    while ((i = find_next_bit((const unsigned long *) &this_cpu(lr_mask),
                              nr_lrs, i)) < nr_lrs ) {
        gic_update_one_lr(v, i);
        i++;
    }

    spin_unlock_irqrestore(&v->arch.vgic.lock, flags);
}

static void gic_restore_pending_irqs(struct vcpu *v)
{
    int lr = 0;
    struct pending_irq *p, *t, *p_r;
    struct list_head *inflight_r;
    unsigned long flags;
    unsigned int nr_lrs = gic_hw_ops->info->nr_lrs;
    int lrs = nr_lrs;

    spin_lock_irqsave(&v->arch.vgic.lock, flags);

    if ( list_empty(&v->arch.vgic.lr_pending) )
        goto out;

    inflight_r = &v->arch.vgic.inflight_irqs;
    list_for_each_entry_safe ( p, t, &v->arch.vgic.lr_pending, lr_queue )
    {
        lr = find_next_zero_bit(&this_cpu(lr_mask), nr_lrs, lr);
        if ( lr >= nr_lrs )
        {
            /* No more free LRs: find a lower priority irq to evict */
            list_for_each_entry_reverse( p_r, inflight_r, inflight )
            {
                if ( p_r->priority == p->priority )
                    goto out;
                if ( test_bit(GIC_IRQ_GUEST_VISIBLE, &p_r->status) &&
                     !test_bit(GIC_IRQ_GUEST_ACTIVE, &p_r->status) )
                    goto found;
            }
            /* We didn't find a victim this time, and we won't next
             * time, so quit */
            goto out;

found:
            lr = p_r->lr;
            p_r->lr = GIC_INVALID_LR;
            set_bit(GIC_IRQ_GUEST_QUEUED, &p_r->status);
            clear_bit(GIC_IRQ_GUEST_VISIBLE, &p_r->status);
            gic_add_to_lr_pending(v, p_r);
            inflight_r = &p_r->inflight;
        }

        gic_set_lr(lr, p, GICH_LR_PENDING);
        list_del_init(&p->lr_queue);
        set_bit(lr, &this_cpu(lr_mask));

        /* We can only evict nr_lrs entries */
        lrs--;
        if ( lrs == 0 )
            break;
    }

out:
    spin_unlock_irqrestore(&v->arch.vgic.lock, flags);
}

void gic_clear_pending_irqs(struct vcpu *v)
{
    struct pending_irq *p, *t;

    ASSERT(spin_is_locked(&v->arch.vgic.lock));

    v->arch.lr_mask = 0;
    list_for_each_entry_safe ( p, t, &v->arch.vgic.lr_pending, lr_queue )
        list_del_init(&p->lr_queue);
}

int gic_events_need_delivery(void)
{
    struct vcpu *v = current;
    struct pending_irq *p;
    unsigned long flags;
    const unsigned long apr = gic_hw_ops->read_apr(0);
    int mask_priority;
    int active_priority;
    int rc = 0;

    mask_priority = gic_hw_ops->read_vmcr_priority();
    active_priority = find_next_bit(&apr, 32, 0);

    spin_lock_irqsave(&v->arch.vgic.lock, flags);

    /* TODO: We order the guest irqs by priority, but we don't change
     * the priority of host irqs. */

    /* find the first enabled non-active irq, the queue is already
     * ordered by priority */
    list_for_each_entry( p, &v->arch.vgic.inflight_irqs, inflight )
    {
        if ( GIC_PRI_TO_GUEST(p->priority) >= mask_priority )
            goto out;
        if ( GIC_PRI_TO_GUEST(p->priority) >= active_priority )
            goto out;
        if ( test_bit(GIC_IRQ_GUEST_ENABLED, &p->status) )
        {
            rc = 1;
            goto out;
        }
    }

out:
    spin_unlock_irqrestore(&v->arch.vgic.lock, flags);
    return rc;
}

void gic_inject(void)
{
    ASSERT(!local_irq_is_enabled());

    gic_restore_pending_irqs(current);

    if ( !list_empty(&current->arch.vgic.lr_pending) && lr_all_full() )
        gic_hw_ops->update_hcr_status(GICH_HCR_UIE, 1);
    else
        gic_hw_ops->update_hcr_status(GICH_HCR_UIE, 0);
}

static void do_sgi(struct cpu_user_regs *regs, enum gic_sgi sgi)
{
    /* Lower the priority */
    struct irq_desc *desc = irq_to_desc(sgi);

    /* Lower the priority */
    gic_hw_ops->eoi_irq(desc);

    switch (sgi)
    {
    case GIC_SGI_EVENT_CHECK:
        /* Nothing to do, will check for events on return path */
        break;
    case GIC_SGI_DUMP_STATE:
        dump_execstate(regs);
        break;
    case GIC_SGI_CALL_FUNCTION:
        smp_call_function_interrupt();
        break;
    default:
        panic("Unhandled SGI %d on CPU%d", sgi, smp_processor_id());
        break;
    }

    /* Deactivate */
    gic_hw_ops->deactivate_irq(desc);
}

/* Accept an interrupt from the GIC and dispatch its handler */
void gic_interrupt(struct cpu_user_regs *regs, int is_fiq)
{
    unsigned int irq;

    do  {
        /* Reading IRQ will ACK it */
        irq = gic_hw_ops->read_irq();

        if ( likely(irq >= 16 && irq < 1021) )
        {
            local_irq_enable();
            do_IRQ(regs, irq, is_fiq);
            local_irq_disable();
        }
        else if (unlikely(irq < 16))
        {
            do_sgi(regs, irq);
        }
        else
        {
            local_irq_disable();
            break;
        }
    } while (1);
}

int gicv_setup(struct domain *d)
{
    return gic_hw_ops->gicv_setup(d);
}

static void maintenance_interrupt(int irq, void *dev_id, struct cpu_user_regs *regs)
{
    /*
     * This is a dummy interrupt handler.
     * Receiving the interrupt is going to cause gic_inject to be called
     * on return to guest that is going to clear the old LRs and inject
     * new interrupts.
     */
}

void gic_dump_info(struct vcpu *v)
{
    struct pending_irq *p;

    printk("GICH_LRs (vcpu %d) mask=%"PRIx64"\n", v->vcpu_id, v->arch.lr_mask);
    gic_hw_ops->dump_state(v);

    list_for_each_entry ( p, &v->arch.vgic.inflight_irqs, inflight )
    {
        printk("Inflight irq=%d lr=%u\n", p->irq, p->lr);
    }

    list_for_each_entry( p, &v->arch.vgic.lr_pending, lr_queue )
    {
        printk("Pending irq=%d\n", p->irq);
    }
}

void __cpuinit init_maintenance_interrupt(void)
{
    request_irq(gic_hw_ops->info->maintenance_irq, 0, maintenance_interrupt,
                "irq-maintenance", NULL);
}

int gic_make_node(const struct domain *d,const struct dt_device_node *node,
                   void *fdt)
{
    return gic_hw_ops->make_dt_node(d, node, fdt);
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
