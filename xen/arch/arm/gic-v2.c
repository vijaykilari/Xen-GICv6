/*
 * xen/arch/arm/gic-v2.c
 *
 * ARM Generic Interrupt Controller support v2
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

/*
 * LR register definitions are GIC v2 specific.
 * Moved these definitions from header file to here
 */
#define GICH_V2_LR_VIRTUAL_MASK    0x3ff
#define GICH_V2_LR_VIRTUAL_SHIFT   0
#define GICH_V2_LR_PHYSICAL_MASK   0x3ff
#define GICH_V2_LR_PHYSICAL_SHIFT  10
#define GICH_V2_LR_STATE_MASK      0x3
#define GICH_V2_LR_STATE_SHIFT     28
#define GICH_V2_LR_PRIORITY_SHIFT  23
#define GICH_V2_LR_PRIORITY_MASK   0x1f
#define GICH_V2_LR_HW_SHIFT        31
#define GICH_V2_LR_HW_MASK         0x1
#define GICH_V2_LR_GRP_SHIFT       30
#define GICH_V2_LR_GRP_MASK        0x1
#define GICH_V2_LR_MAINTENANCE_IRQ (1<<19)
#define GICH_V2_LR_GRP1            (1<<30)
#define GICH_V2_LR_HW              (1<<31)
#define GICH_V2_LR_CPUID_SHIFT     9
#define GICH_V2_VTR_NRLRGS         0x3f

#define GICH_V2_VMCR_PRIORITY_MASK   0x1f
#define GICH_V2_VMCR_PRIORITY_SHIFT  27

#define GICD (gicv2.map_dbase)
#define GICC (gicv2.map_cbase)
#define GICH (gicv2.map_hbase)

/* Global state */
static struct {
    paddr_t dbase;            /* Address of distributor registers */
    void __iomem * map_dbase; /* IO mapped Address of distributor registers */
    paddr_t cbase;            /* Address of CPU interface registers */
    void __iomem * map_cbase; /* IO mapped Address of CPU interface registers*/
    paddr_t hbase;            /* Address of virtual interface registers */
    void __iomem * map_hbase; /* IO Address of virtual interface registers */
    paddr_t vbase;            /* Address of virtual cpu interface registers */
    spinlock_t lock;
} gicv2;

static struct gic_info gicv2_info;

/* The GIC mapping of CPU interfaces does not necessarily match the
 * logical CPU numbering. Let's use mapping as returned by the GIC
 * itself
 */
static DEFINE_PER_CPU(u8, gic_cpu_id);

/* Maximum cpu interface per GIC */
#define NR_GIC_CPU_IF 8

static unsigned int gicv2_cpu_mask(const cpumask_t *cpumask)
{
    unsigned int cpu;
    unsigned int mask = 0;
    cpumask_t possible_mask;

    cpumask_and(&possible_mask, cpumask, &cpu_possible_map);
    for_each_cpu( cpu, &possible_mask )
    {
        ASSERT(cpu < NR_GIC_CPU_IF);
        mask |= per_cpu(gic_cpu_id, cpu);
    }

    return mask;
}

static void gicv2_save_state(struct vcpu *v)
{
    int i;

    /* No need for spinlocks here because interrupts are disabled around
     * this call and it only accesses struct vcpu fields that cannot be
     * accessed simultaneously by another pCPU.
     */
    for ( i = 0; i < gicv2_info.nr_lrs; i++ )
        v->arch.gic.v2.lr[i] = readl_relaxed(GICH + GICH_LR + i * 4);

    v->arch.gic.v2.apr = readl_relaxed(GICH + GICH_APR);
    v->arch.gic.v2.vmcr = readl_relaxed(GICH + GICH_VMCR);
    /* Disable until next VCPU scheduled */
    writel_relaxed(0, GICH + GICH_HCR);
}

static void gicv2_restore_state(const struct vcpu *v)
{
    int i;

    for ( i = 0; i < gicv2_info.nr_lrs; i++ )
        writel_relaxed(v->arch.gic.v2.lr[i], GICH + GICH_LR + i * 4);

    writel_relaxed(v->arch.gic.v2.apr, GICH + GICH_APR);
    writel_relaxed(v->arch.gic.v2.vmcr, GICH + GICH_VMCR);
    writel_relaxed(GICH_HCR_EN, GICH + GICH_HCR);
}

static void gicv2_dump_state(const struct vcpu *v)
{
    int i;

    if ( v == current )
    {
        for ( i = 0; i < gicv2_info.nr_lrs; i++ )
            printk("   HW_LR[%d]=%x\n", i,
                   readl_relaxed(GICH + GICH_LR + i * 4));
    }
    else
    {
        for ( i = 0; i < gicv2_info.nr_lrs; i++ )
            printk("   VCPU_LR[%d]=%x\n", i, v->arch.gic.v2.lr[i]);
    }
}

static void gicv2_eoi_irq(struct irq_desc *irqd)
{
    int irq = irqd->irq;
    /* Lower the priority */
    writel_relaxed(irq, GICC + GICC_EOIR);
}

static void gicv2_dir_irq(struct irq_desc *irqd)
{
    /* Deactivate */
    writel_relaxed(irqd->irq, GICC + GICC_DIR);
}

static unsigned int gicv2_read_irq(void)
{
    return (readl_relaxed(GICC + GICC_IAR) & GICC_IA_IRQ);
}

/*
 * needs to be called with a valid cpu_mask, ie each cpu in the mask has
 * already called gic_cpu_init
 */
static void gicv2_set_irq_properties(struct irq_desc *desc,
                                   const cpumask_t *cpu_mask,
                                   unsigned int priority)
{
    uint32_t cfg, edgebit, itarget, ipriority;
    unsigned int mask = gicv2_cpu_mask(cpu_mask);
    unsigned int irq = desc->irq;
    unsigned int type = desc->arch.type;

    ASSERT(type != DT_IRQ_TYPE_INVALID);
    ASSERT(spin_is_locked(&desc->lock));

    spin_lock(&gicv2.lock);
    /* Set edge / level */
    cfg = readl_relaxed(GICD + GICD_ICFGR + (irq / 16) * 4);
    edgebit = 2u << (2 * (irq % 16));
    if ( type & DT_IRQ_TYPE_LEVEL_MASK )
        cfg &= ~edgebit;
    else if ( type & DT_IRQ_TYPE_EDGE_BOTH )
        cfg |= edgebit;
    writel_relaxed(cfg, GICD + GICD_ICFGR + (irq / 16) * 4);

    /* Set target CPU mask (RAZ/WI on uniprocessor) */
    itarget = readl_relaxed(GICD + GICD_ITARGETSR + (irq / 4) * 4);
    /* Clear mask */
    itarget &= ~(0xffu << (8 * (irq % 4)));
    /* Set mask */
    itarget |=  mask << (8 * (irq % 4));
    writel_relaxed(itarget, GICD + GICD_ITARGETSR + (irq / 4) * 4);

    ipriority = readl_relaxed(GICD + GICD_IPRIORITYR + (irq / 4) * 4);
    /* Clear priority */
    ipriority &= ~(0xffu << (8 * (irq % 4)));
    /* Set priority */
    ipriority |=  priority << (8 * (irq % 4));
    writel_relaxed(ipriority, GICD + GICD_IPRIORITYR + (irq / 4) * 4);

    spin_unlock(&gicv2.lock);
}

static void __init gicv2_dist_init(void)
{
    uint32_t type;
    uint32_t cpumask;
    uint32_t gic_cpus;
    int i;

    cpumask = readl_relaxed(GICD + GICD_ITARGETSR) & 0xff;
    cpumask |= cpumask << 8;
    cpumask |= cpumask << 16;

    /* Disable the distributor */
    writel_relaxed(0, GICD + GICD_CTLR);

    type = readl_relaxed(GICD + GICD_TYPER);
    gicv2_info.nr_lines = 32 * ((type & GICD_TYPE_LINES) + 1);
    gic_cpus = 1 + ((type & GICD_TYPE_CPUS) >> 5);
    printk("GICv2: %d lines, %d cpu%s%s (IID %8.8x).\n",
           gicv2_info.nr_lines, gic_cpus, (gic_cpus == 1) ? "" : "s",
           (type & GICD_TYPE_SEC) ? ", secure" : "",
           readl_relaxed(GICD + GICD_IIDR));

    /* Default all global IRQs to level, active low */
    for ( i = 32; i < gicv2_info.nr_lines; i += 16 )
        writel_relaxed(0x0, GICD + GICD_ICFGR + (i / 16) * 4);

    /* Route all global IRQs to this CPU */
    for ( i = 32; i < gicv2_info.nr_lines; i += 4 )
        writel_relaxed(cpumask, GICD + GICD_ITARGETSR + (i / 4) * 4);

    /* Default priority for global interrupts */
    for ( i = 32; i < gicv2_info.nr_lines; i += 4 )
        writel_relaxed (GIC_PRI_IRQ << 24 | GIC_PRI_IRQ << 16 |
                        GIC_PRI_IRQ << 8 | GIC_PRI_IRQ,
                        GICD + GICD_IPRIORITYR + (i / 4) * 4);

    /* Disable all global interrupts */
    for ( i = 32; i < gicv2_info.nr_lines; i += 32 )
        writel_relaxed(~0x0, GICD + GICD_ICENABLER + (i / 32) * 4);

    /* Turn on the distributor */
    writel_relaxed(GICD_CTL_ENABLE, GICD + GICD_CTLR);
}

static void __cpuinit gicv2_cpu_init(void)
{
    int i;

    this_cpu(gic_cpu_id) = readl_relaxed(GICD + GICD_ITARGETSR) & 0xff;

    /* The first 32 interrupts (PPI and SGI) are banked per-cpu, so
     * even though they are controlled with GICD registers, they must
     * be set up here with the other per-cpu state. */
    writel_relaxed(0xffff0000, GICD + GICD_ICENABLER); /* Disable all PPI */
    writel_relaxed(0x0000ffff, GICD + GICD_ISENABLER); /* Enable all SGI */

    /* Set SGI priorities */
    for ( i = 0; i < 16; i += 4 )
        writel_relaxed(GIC_PRI_IPI << 24 | GIC_PRI_IPI << 16 |
                       GIC_PRI_IPI << 8 | GIC_PRI_IPI,
                       GICD + GICD_IPRIORITYR + (i / 4) * 4);

    /* Set PPI priorities */
    for ( i = 16; i < 32; i += 4 )
        writel_relaxed(GIC_PRI_IRQ << 24 | GIC_PRI_IRQ << 16 |
                      GIC_PRI_IRQ << 8 | GIC_PRI_IRQ,
                      GICD + GICD_IPRIORITYR + (i / 4) * 4);

    /* Local settings: interface controller */
    /* Don't mask by priority */
    writel_relaxed(0xff, GICC + GICC_PMR);
    /* Finest granularity of priority */
    writel_relaxed(0x0, GICC + GICC_BPR);
    /* Turn on delivery */
    writel_relaxed(GICC_CTL_ENABLE|GICC_CTL_EOI, GICC + GICC_CTLR);
}

static void gicv2_cpu_disable(void)
{
    writel_relaxed(0x0, GICC + GICC_CTLR);
}

static void __cpuinit gicv2_hyp_init(void)
{
    uint32_t vtr;
    uint8_t nr_lrs;

    vtr = readl_relaxed(GICH + GICH_VTR);
    nr_lrs  = (vtr & GICH_V2_VTR_NRLRGS) + 1;
    gicv2_info.nr_lrs = nr_lrs;

    writel_relaxed(GICH_MISR_EOI, GICH + GICH_MISR);
}

static void __cpuinit gicv2_hyp_disable(void)
{
    writel_relaxed(0, GICH + GICH_HCR);
}

static int gicv2_secondary_cpu_init(void)
{
    spin_lock(&gicv2.lock);

    gicv2_cpu_init();
    gicv2_hyp_init();

    spin_unlock(&gicv2.lock);

    return 0;
}

static void gicv2_send_SGI(enum gic_sgi sgi, enum gic_sgi_mode irqmode,
                           const cpumask_t *cpu_mask)
{
    unsigned int mask = 0;

    switch ( irqmode )
    {
    case SGI_TARGET_OTHERS:
        writel_relaxed(GICD_SGI_TARGET_OTHERS | sgi, GICD + GICD_SGIR);
        break;
    case SGI_TARGET_SELF:
        writel_relaxed(GICD_SGI_TARGET_SELF | sgi, GICD + GICD_SGIR);
        break;
    case SGI_TARGET_LIST:
        mask = gicv2_cpu_mask(cpu_mask);
        writel_relaxed(GICD_SGI_TARGET_LIST |
                       (mask << GICD_SGI_TARGET_SHIFT) | sgi,
                       GICD + GICD_SGIR);
        break;
    default:
        BUG();
    }
}

/* Shut down the per-CPU GIC interface */
static void gicv2_disable_interface(void)
{
    spin_lock(&gicv2.lock);
    gicv2_cpu_disable();
    gicv2_hyp_disable();
    spin_unlock(&gicv2.lock);
}

static void gicv2_update_lr(int lr, const struct pending_irq *p,
                            unsigned int state)
{
    uint32_t lr_reg;

    BUG_ON(lr >= gicv2_info.nr_lrs);
    BUG_ON(lr < 0);

    lr_reg = (((state & GICH_V2_LR_STATE_MASK) << GICH_V2_LR_STATE_SHIFT)  |
              ((GIC_PRI_TO_GUEST(p->priority) & GICH_V2_LR_PRIORITY_MASK)
                                             << GICH_V2_LR_PRIORITY_SHIFT) |
              ((p->irq & GICH_V2_LR_VIRTUAL_MASK) << GICH_V2_LR_VIRTUAL_SHIFT));

    if ( p->desc != NULL )
        lr_reg |= GICH_V2_LR_HW | ((p->desc->irq & GICH_V2_LR_PHYSICAL_MASK )
                                  << GICH_V2_LR_PHYSICAL_SHIFT);

    writel_relaxed(lr_reg, GICH + GICH_LR + lr * 4);
}

static void gicv2_clear_lr(int lr)
{
    writel_relaxed(0, GICH + GICH_LR + lr * 4);
}

static int gicv_v2_init(struct domain *d)
{
    int ret;

    /*
     * Domain 0 gets the hardware address.
     * Guests get the virtual platform layout.
     */
    if ( is_hardware_domain(d) )
    {
        d->arch.vgic.dbase = gicv2.dbase;
        d->arch.vgic.cbase = gicv2.cbase;
    }
    else
    {
        d->arch.vgic.dbase = GUEST_GICD_BASE;
        d->arch.vgic.cbase = GUEST_GICC_BASE;
    }

    d->arch.vgic.nr_lines = 0;

    /*
     * Map the gic virtual cpu interface in the gic cpu interface
     * region of the guest.
     *
     * The second page is always mapped at +4K irrespective of the
     * GIC_64K_STRIDE quirk. The DTB passed to the guest reflects this.
     */
    ret = map_mmio_regions(d, d->arch.vgic.cbase,
                           d->arch.vgic.cbase + PAGE_SIZE - 1,
                           gicv2.vbase);
    if ( ret )
        return ret;

    if ( !platform_has_quirk(PLATFORM_QUIRK_GIC_64K_STRIDE) )
        ret = map_mmio_regions(d, d->arch.vgic.cbase + PAGE_SIZE,
                               d->arch.vgic.cbase + (2 * PAGE_SIZE) - 1,
                               gicv2.vbase + PAGE_SIZE);
    else
        ret = map_mmio_regions(d, d->arch.vgic.cbase + PAGE_SIZE,
                               d->arch.vgic.cbase + (2 * PAGE_SIZE) - 1,
                               gicv2.vbase + 16*PAGE_SIZE);

    return ret;
}

static void gicv2_read_lr(int lr, struct gic_lr *lr_reg)
{
    uint32_t lrv;

    lrv          = readl_relaxed(GICH + GICH_LR + lr * 4);
    lr_reg->pirq = (lrv >> GICH_V2_LR_PHYSICAL_SHIFT) & GICH_V2_LR_PHYSICAL_MASK;
    lr_reg->virq = (lrv >> GICH_V2_LR_VIRTUAL_SHIFT) & GICH_V2_LR_VIRTUAL_MASK;
    lr_reg->priority = (lrv >> GICH_V2_LR_PRIORITY_SHIFT) & GICH_V2_LR_PRIORITY_MASK;
    lr_reg->state     = (lrv >> GICH_V2_LR_STATE_SHIFT) & GICH_V2_LR_STATE_MASK;
    lr_reg->hw_status = (lrv >> GICH_V2_LR_HW_SHIFT) & GICH_V2_LR_HW_MASK;
    lr_reg->grp       = (lrv >> GICH_V2_LR_GRP_SHIFT) & GICH_V2_LR_GRP_MASK;
}

static void gicv2_write_lr(int lr, const struct gic_lr *lr_reg)
{
    uint32_t lrv = 0;

    lrv = ( ((lr_reg->pirq & GICH_V2_LR_PHYSICAL_MASK) << GICH_V2_LR_PHYSICAL_SHIFT) |
          ((lr_reg->virq & GICH_V2_LR_VIRTUAL_MASK) << GICH_V2_LR_VIRTUAL_SHIFT)   |
          ((uint32_t)(lr_reg->priority & GICH_V2_LR_PRIORITY_MASK)
                                      << GICH_V2_LR_PRIORITY_SHIFT) |
          ((uint32_t)(lr_reg->state & GICH_V2_LR_STATE_MASK)
                                   << GICH_V2_LR_STATE_SHIFT) |
          ((uint32_t)(lr_reg->hw_status & GICH_V2_LR_HW_MASK)
                                       << GICH_V2_LR_HW_SHIFT)  |
          ((uint32_t)(lr_reg->grp & GICH_V2_LR_GRP_MASK) << GICH_V2_LR_GRP_SHIFT) );

    writel_relaxed(lrv, GICH + GICH_LR + lr * 4);
}

static void gicv2_hcr_status(uint32_t flag, bool_t status)
{
    uint32_t hcr = readl_relaxed(GICH + GICH_HCR);

    if ( status )
        hcr |= flag;
    else
        hcr &= (~flag);

    writel_relaxed(hcr, GICH + GICH_HCR);
}

static unsigned int gicv2_read_vmcr_priority(void)
{
   return ((readl_relaxed(GICH + GICH_VMCR) >> GICH_V2_VMCR_PRIORITY_SHIFT)
           & GICH_V2_VMCR_PRIORITY_MASK);
}

static unsigned int gicv2_read_apr(int apr_reg)
{
   return readl_relaxed(GICH + GICH_APR);
}

static void gicv2_irq_enable(struct irq_desc *desc)
{
    unsigned long flags;
    int irq = desc->irq;

    ASSERT(spin_is_locked(&desc->lock));

    spin_lock_irqsave(&gicv2.lock, flags);
    desc->status &= ~IRQ_DISABLED;
    dsb(sy);
    /* Enable routing */
    writel_relaxed((1u << (irq % 32)), GICD + GICD_ISENABLER + (irq / 32) * 4);
    spin_unlock_irqrestore(&gicv2.lock, flags);
}

static void gicv2_irq_disable(struct irq_desc *desc)
{
    unsigned long flags;
    int irq = desc->irq;

    ASSERT(spin_is_locked(&desc->lock));

    spin_lock_irqsave(&gicv2.lock, flags);
    /* Disable routing */
    writel_relaxed(1u << (irq % 32), GICD + GICD_ICENABLER + (irq / 32) * 4);
    desc->status |= IRQ_DISABLED;
    spin_unlock_irqrestore(&gicv2.lock, flags);
}

static unsigned int gicv2_irq_startup(struct irq_desc *desc)
{
    gicv2_irq_enable(desc);

    return 0;
}

static void gicv2_irq_shutdown(struct irq_desc *desc)
{
    gicv2_irq_disable(desc);
}

static void gicv2_irq_ack(struct irq_desc *desc)
{
    /* No ACK -- reading IAR has done this for us */
}

static void gicv2_host_irq_end(struct irq_desc *desc)
{
    /* Lower the priority */
    gicv2_eoi_irq(desc);
    /* Deactivate */
    gicv2_dir_irq(desc);
}

static void gicv2_guest_irq_end(struct irq_desc *desc)
{
    /* Lower the priority of the IRQ */
    gicv2_eoi_irq(desc);
    /* Deactivation happens in maintenance interrupt / via GICV */
}

static void gicv2_irq_set_affinity(struct irq_desc *desc, const cpumask_t *mask)
{
    BUG();
}

/* XXX different for level vs edge */
static hw_irq_controller gicv2_host_irq_type = {
    .typename     = "gic-v2",
    .startup      = gicv2_irq_startup,
    .shutdown     = gicv2_irq_shutdown,
    .enable       = gicv2_irq_enable,
    .disable      = gicv2_irq_disable,
    .ack          = gicv2_irq_ack,
    .end          = gicv2_host_irq_end,
    .set_affinity = gicv2_irq_set_affinity,
};

static hw_irq_controller gicv2_guest_irq_type = {
    .typename     = "gic-v2",
    .startup      = gicv2_irq_startup,
    .shutdown     = gicv2_irq_shutdown,
    .enable       = gicv2_irq_enable,
    .disable      = gicv2_irq_disable,
    .ack          = gicv2_irq_ack,
    .end          = gicv2_guest_irq_end,
    .set_affinity = gicv2_irq_set_affinity,
};

const static struct gic_hw_operations gicv2_ops = {
    .info                = &gicv2_info,
    .secondary_init      = gicv2_secondary_cpu_init,
    .save_state          = gicv2_save_state,
    .restore_state       = gicv2_restore_state,
    .dump_state          = gicv2_dump_state,
    .gicv_setup          = gicv_v2_init,
    .gic_host_irq_type   = &gicv2_host_irq_type,
    .gic_guest_irq_type  = &gicv2_guest_irq_type,
    .eoi_irq             = gicv2_eoi_irq,
    .deactivate_irq      = gicv2_dir_irq,
    .read_irq            = gicv2_read_irq,
    .set_irq_properties  = gicv2_set_irq_properties,
    .send_SGI            = gicv2_send_SGI,
    .disable_interface   = gicv2_disable_interface,
    .update_lr           = gicv2_update_lr,
    .update_hcr_status   = gicv2_hcr_status,
    .clear_lr            = gicv2_clear_lr,
    .read_lr             = gicv2_read_lr,
    .write_lr            = gicv2_write_lr,
    .read_vmcr_priority  = gicv2_read_vmcr_priority,
    .read_apr            = gicv2_read_apr,
};

/* Set up the GIC */
static int __init gicv2_init(struct dt_device_node *node, const void *data)
{
    int res;

    dt_device_set_used_by(node, DOMID_XEN);

    res = dt_device_get_address(node, 0, &gicv2.dbase, NULL);
    if ( res || !gicv2.dbase || (gicv2.dbase & ~PAGE_MASK) )
        panic("GICv2: Cannot find a valid address for the distributor");

    res = dt_device_get_address(node, 1, &gicv2.cbase, NULL);
    if ( res || !gicv2.cbase || (gicv2.cbase & ~PAGE_MASK) )
        panic("GICv2: Cannot find a valid address for the CPU");

    res = dt_device_get_address(node, 2, &gicv2.hbase, NULL);
    if ( res || !gicv2.hbase || (gicv2.hbase & ~PAGE_MASK) )
        panic("GICv2: Cannot find a valid address for the hypervisor");

    res = dt_device_get_address(node, 3, &gicv2.vbase, NULL);
    if ( res || !gicv2.vbase || (gicv2.vbase & ~PAGE_MASK) )
        panic("GICv2: Cannot find a valid address for the virtual CPU");

    res = platform_get_irq(node, 0);
    if ( res < 0 )
        panic("GICv2: Cannot find the maintenance IRQ");
    gicv2_info.maintenance_irq = res;

    /* Set the GIC as the primary interrupt controller */
    dt_interrupt_controller = node;

    /* TODO: Add check on distributor, cpu size */

    printk("GICv2 initialization:\n"
              "        gic_dist_addr=%"PRIpaddr"\n"
              "        gic_cpu_addr=%"PRIpaddr"\n"
              "        gic_hyp_addr=%"PRIpaddr"\n"
              "        gic_vcpu_addr=%"PRIpaddr"\n"
              "        gic_maintenance_irq=%u\n",
              gicv2.dbase, gicv2.cbase, gicv2.hbase, gicv2.vbase,
              gicv2_info.maintenance_irq);

    if ( (gicv2.dbase & ~PAGE_MASK) || (gicv2.cbase & ~PAGE_MASK) ||
         (gicv2.hbase & ~PAGE_MASK) || (gicv2.vbase & ~PAGE_MASK) )
        panic("GICv2 interfaces not page aligned");

    gicv2.map_dbase = ioremap_nocache(gicv2.dbase, PAGE_SIZE);
    if ( !gicv2.map_dbase )
        panic("GICv2: Failed to ioremap for GIC distributor\n");

    if ( platform_has_quirk(PLATFORM_QUIRK_GIC_64K_STRIDE) )
        gicv2.map_cbase = ioremap_nocache(gicv2.cbase, PAGE_SIZE * 0x10);
    else
        gicv2.map_cbase = ioremap_nocache(gicv2.cbase, PAGE_SIZE * 2);

    if ( !gicv2.map_cbase )
        panic("GICv2: Failed to ioremap for GIC CPU interface\n");

    gicv2.map_hbase = ioremap_nocache(gicv2.hbase, PAGE_SIZE);
    if ( !gicv2.map_hbase )
        panic("GICv2: Failed to ioremap for GIC Virtual interface\n");

    /* Global settings: interrupt distributor */
    spin_lock_init(&gicv2.lock);
    spin_lock(&gicv2.lock);

    gicv2_dist_init();
    gicv2_cpu_init();
    gicv2_hyp_init();

    spin_unlock(&gicv2.lock);

    gicv2_info.hw_version = GIC_V2;
    register_gic_ops(&gicv2_ops);

    return 0;
}

static const char * const gicv2_dt_compat[] __initconst =
{
    DT_COMPAT_GIC_CORTEX_A15,
    DT_COMPAT_GIC_CORTEX_A7,
    NULL
};

DT_DEVICE_START(gicv2, "GICv2:", DEVICE_GIC)
        .compatible = gicv2_dt_compat,
        .init = gicv2_init,
DT_DEVICE_END

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
