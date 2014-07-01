/*
 * xen/arch/arm/vgic-v2.c
 *
 * ARM Virtual Generic Interrupt Controller support v2
 *
 * Ian Campbell <ian.campbell@citrix.com>
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

#include <xen/bitops.h>
#include <xen/config.h>
#include <xen/lib.h>
#include <xen/init.h>
#include <xen/softirq.h>
#include <xen/irq.h>
#include <xen/sched.h>

#include <asm/current.h>
#include <asm/device.h>

#include <asm/mmio.h>
#include <asm/gic.h>
#include <asm/vgic.h>

static int vgic_v2_distr_mmio_read(struct vcpu *v, mmio_info_t *info)
{
    struct hsr_dabt dabt = info->dabt;
    struct cpu_user_regs *regs = guest_cpu_user_regs();
    register_t *r = select_user_reg(regs, dabt.reg);
    struct vgic_irq_rank *rank;
    int gicd_reg = (int)(info->gpa - v->domain->arch.vgic.dbase);

    switch ( gicd_reg )
    {
    case GICD_CTLR:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        vgic_lock(v);
        *r = v->domain->arch.vgic.ctlr;
        vgic_unlock(v);
        return 1;
    case GICD_TYPER:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        /* No secure world support for guests. */
        vgic_lock(v);
        *r = ( (v->domain->max_vcpus << 5) & GICD_TYPE_CPUS )
            |( ((v->domain->arch.vgic.nr_lines / 32)) & GICD_TYPE_LINES );
        vgic_unlock(v);
        return 1;
    case GICD_IIDR:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        /*
         * XXX Do we need a JEP106 manufacturer ID?
         * Just use the physical h/w value for now
         */
        *r = 0x0000043b;
        return 1;

    /* Implementation defined -- read as zero */
    case 0x020 ... 0x03c:
        goto read_as_zero;

    case GICD_IGROUPR ... GICD_IGROUPRN:
        /* We do not implement security extensions for guests, read zero */
        goto read_as_zero;

    case GICD_ISENABLER ... GICD_ISENABLERN:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, gicd_reg - GICD_ISENABLER, DABT_WORD);
        if ( rank == NULL) goto read_as_zero;
        vgic_lock_rank(v, rank);
        *r = rank->ienable;
        vgic_unlock_rank(v, rank);
        return 1;

    case GICD_ICENABLER ... GICD_ICENABLERN:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, gicd_reg - GICD_ICENABLER, DABT_WORD);
        if ( rank == NULL) goto read_as_zero;
        vgic_lock_rank(v, rank);
        *r = rank->ienable;
        vgic_unlock_rank(v, rank);
        return 1;

    case GICD_ISPENDR ... GICD_ISPENDRN:
        if ( dabt.size != DABT_BYTE && dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, gicd_reg - GICD_ISPENDR, DABT_WORD);
        if ( rank == NULL) goto read_as_zero;
        vgic_lock_rank(v, rank);
        *r = vgic_byte_read(rank->ipend, dabt.sign, gicd_reg);
        vgic_unlock_rank(v, rank);
        return 1;

    case GICD_ICPENDR ... GICD_ICPENDRN:
        if ( dabt.size != DABT_BYTE && dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, gicd_reg - GICD_ICPENDR, DABT_WORD);
        if ( rank == NULL) goto read_as_zero;
        vgic_lock_rank(v, rank);
        *r = vgic_byte_read(rank->ipend, dabt.sign, gicd_reg);
        vgic_unlock_rank(v, rank);
        return 1;

    case GICD_ISACTIVER ... GICD_ISACTIVERN:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, gicd_reg - GICD_ISACTIVER, DABT_WORD);
        if ( rank == NULL) goto read_as_zero;
        vgic_lock_rank(v, rank);
        *r = rank->iactive;
        vgic_unlock_rank(v, rank);
        return 1;

    case GICD_ICACTIVER ... GICD_ICACTIVERN:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, gicd_reg - GICD_ICACTIVER, DABT_WORD);
        if ( rank == NULL) goto read_as_zero;
        vgic_lock_rank(v, rank);
        *r = rank->iactive;
        vgic_unlock_rank(v, rank);
        return 1;

    case GICD_ITARGETSR ... GICD_ITARGETSRN:
        if ( dabt.size != DABT_BYTE && dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 8, gicd_reg - GICD_ITARGETSR, DABT_WORD);
        if ( rank == NULL) goto read_as_zero;

        vgic_lock_rank(v, rank);
        *r = rank->v2.itargets[REG_RANK_INDEX(8, gicd_reg - GICD_ITARGETSR,
                                           DABT_WORD)];
        if ( dabt.size == DABT_BYTE )
            *r = vgic_byte_read(*r, dabt.sign, gicd_reg);
        vgic_unlock_rank(v, rank);
        return 1;

    case GICD_IPRIORITYR ... GICD_IPRIORITYRN:
        if ( dabt.size != DABT_BYTE && dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 8, gicd_reg - GICD_IPRIORITYR, DABT_WORD);
        if ( rank == NULL) goto read_as_zero;

        vgic_lock_rank(v, rank);
        *r = rank->ipriority[REG_RANK_INDEX(8, gicd_reg - GICD_IPRIORITYR,
                                            DABT_WORD)];
        if ( dabt.size == DABT_BYTE )
            *r = vgic_byte_read(*r, dabt.sign, gicd_reg);
        vgic_unlock_rank(v, rank);
        return 1;

    case GICD_ICFGR ... GICD_ICFGRN:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 2, gicd_reg - GICD_ICFGR, DABT_WORD);
        if ( rank == NULL) goto read_as_zero;
        vgic_lock_rank(v, rank);
        *r = rank->icfg[REG_RANK_INDEX(2, gicd_reg - GICD_ICFGR, DABT_WORD)];
        vgic_unlock_rank(v, rank);
        return 1;

    case GICD_NSACR ... GICD_NSACRN:
        /* We do not implement security extensions for guests, read zero */
        goto read_as_zero;

    case GICD_SGIR:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        /* Write only -- read unknown */
        *r = 0xdeadbeef;
        return 1;

    case GICD_CPENDSGIR ... GICD_CPENDSGIRN:
        if ( dabt.size != DABT_BYTE && dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, gicd_reg - GICD_CPENDSGIR, DABT_WORD);
        if ( rank == NULL) goto read_as_zero;
        vgic_lock_rank(v, rank);
        *r = vgic_byte_read(rank->pendsgi, dabt.sign, gicd_reg);
        vgic_unlock_rank(v, rank);
        return 1;

    case GICD_SPENDSGIR ... GICD_SPENDSGIRN:
        if ( dabt.size != DABT_BYTE && dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, gicd_reg - GICD_SPENDSGIR, DABT_WORD);
        if ( rank == NULL) goto read_as_zero;
        vgic_lock_rank(v, rank);
        *r = vgic_byte_read(rank->pendsgi, dabt.sign, gicd_reg);
        vgic_unlock_rank(v, rank);
        return 1;

    /* Implementation defined -- read as zero */
    case 0xfd0 ... 0xfe4:
        goto read_as_zero;

    case GICD_ICPIDR2:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        printk("vGICD: unhandled read from ICPIDR2\n");
        return 0;

    /* Implementation defined -- read as zero */
    case 0xfec ... 0xffc:
        goto read_as_zero;

    /* Reserved -- read as zero */
    case 0x00c ... 0x01c:
    case 0x040 ... 0x07c:
    case 0x7fc:
    case 0xbfc:
    case 0xf04 ... 0xf0c:
    case 0xf30 ... 0xfcc:
        goto read_as_zero;

    default:
        printk("vGICD: unhandled read r%d offset %#08x\n",
               dabt.reg, gicd_reg);
        return 0;
    }

bad_width:
    printk("vGICD: bad read width %d r%d offset %#08x\n",
           dabt.size, dabt.reg, gicd_reg);
    domain_crash_synchronous();
    return 0;

read_as_zero:
    if ( dabt.size != DABT_WORD ) goto bad_width;
    *r = 0;
    return 1;
}

static int vgic_v2_to_sgi(struct vcpu *v, register_t sgir)
{

    int virq;
    int irqmode;
    enum gic_sgi_mode sgi_mode;
    unsigned long vcpu_mask = 0;

    irqmode = (sgir & GICD_SGI_TARGET_LIST_MASK) >> GICD_SGI_TARGET_LIST_SHIFT;
    virq = (sgir & GICD_SGI_INTID_MASK);
    vcpu_mask = (sgir & GICD_SGI_TARGET_MASK) >> GICD_SGI_TARGET_SHIFT;

    /* Map GIC sgi value to enum value */
    switch ( irqmode )
    {
    case GICD_SGI_TARGET_LIST_VAL:
        sgi_mode = SGI_TARGET_LIST;
        break;
    case GICD_SGI_TARGET_OTHERS_VAL:
        sgi_mode = SGI_TARGET_OTHERS;
        break;
    case GICD_SGI_TARGET_SELF_VAL:
        sgi_mode = SGI_TARGET_SELF;
        break;
    default:
        BUG();
    }

    return vgic_to_sgi(v, sgir, sgi_mode, virq, vcpu_mask);
}

static int vgic_v2_distr_mmio_write(struct vcpu *v, mmio_info_t *info)
{
    struct hsr_dabt dabt = info->dabt;
    struct cpu_user_regs *regs = guest_cpu_user_regs();
    register_t *r = select_user_reg(regs, dabt.reg);
    struct vgic_irq_rank *rank;
    int gicd_reg = (int)(info->gpa - v->domain->arch.vgic.dbase);
    uint32_t tr;

    switch ( gicd_reg )
    {
    case GICD_CTLR:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        /* Ignore all but the enable bit */
        v->domain->arch.vgic.ctlr = (*r) & GICD_CTL_ENABLE;
        return 1;

    /* R/O -- write ignored */
    case GICD_TYPER:
    case GICD_IIDR:
        goto write_ignore;

    /* Implementation defined -- write ignored */
    case 0x020 ... 0x03c:
        goto write_ignore;

    case GICD_IGROUPR ... GICD_IGROUPRN:
        /* We do not implement security extensions for guests, write ignore */
        goto write_ignore;

    case GICD_ISENABLER ... GICD_ISENABLERN:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, gicd_reg - GICD_ISENABLER, DABT_WORD);
        if ( rank == NULL) goto write_ignore;
        vgic_lock_rank(v, rank);
        tr = rank->ienable;
        rank->ienable |= *r;
        vgic_unlock_rank(v, rank);
        /* The virtual irq is derived from register offset.
         * The register difference is word difference. So divide by 2(DABT_WORD)
         * to get Virtual irq number */
        vgic_enable_irqs(v, (*r) & (~tr),
                         (gicd_reg - GICD_ISENABLER) >> DABT_WORD);
        return 1;

    case GICD_ICENABLER ... GICD_ICENABLERN:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, gicd_reg - GICD_ICENABLER, DABT_WORD);
        if ( rank == NULL) goto write_ignore;
        vgic_lock_rank(v, rank);
        tr = rank->ienable;
        rank->ienable &= ~*r;
        vgic_unlock_rank(v, rank);
        /* The virtual irq is derived from register offset.
         * The register difference is word difference. So divide by 2(DABT_WORD)
         * to get  Virtual irq number */
        vgic_disable_irqs(v, (*r) & tr,
                         (gicd_reg - GICD_ICENABLER) >> DABT_WORD);
        return 1;

    case GICD_ISPENDR ... GICD_ISPENDRN:
        if ( dabt.size != DABT_BYTE && dabt.size != DABT_WORD ) goto bad_width;
        printk("vGICD: unhandled %s write %#"PRIregister" to ISPENDR%d\n",
               dabt.size ? "word" : "byte", *r, gicd_reg - GICD_ISPENDR);
        return 0;

    case GICD_ICPENDR ... GICD_ICPENDRN:
        if ( dabt.size != DABT_BYTE && dabt.size != DABT_WORD ) goto bad_width;
        printk("vGICD: unhandled %s write %#"PRIregister" to ICPENDR%d\n",
               dabt.size ? "word" : "byte", *r, gicd_reg - GICD_ICPENDR);
        return 0;

    case GICD_ISACTIVER ... GICD_ISACTIVERN:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, gicd_reg - GICD_ISACTIVER, DABT_WORD);
        if ( rank == NULL) goto write_ignore;
        vgic_lock_rank(v, rank);
        rank->iactive &= ~*r;
        vgic_unlock_rank(v, rank);
        return 1;

    case GICD_ICACTIVER ... GICD_ICACTIVERN:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, gicd_reg - GICD_ICACTIVER, DABT_WORD);
        if ( rank == NULL) goto write_ignore;
        vgic_lock_rank(v, rank);
        rank->iactive &= ~*r;
        vgic_unlock_rank(v, rank);
        return 1;

    case GICD_ITARGETSR ... GICD_ITARGETSR + 7:
        /* SGI/PPI target is read only */
        goto write_ignore;

    case GICD_ITARGETSR + 8 ... GICD_ITARGETSRN:
        if ( dabt.size != DABT_BYTE && dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 8, gicd_reg - GICD_ITARGETSR, DABT_WORD);
        if ( rank == NULL) goto write_ignore;
        vgic_lock_rank(v, rank);
        if ( dabt.size == DABT_WORD )
            rank->v2.itargets[REG_RANK_INDEX(8, gicd_reg - GICD_ITARGETSR,
                                          DABT_WORD)] = *r;
        else
            vgic_byte_write(&rank->v2.itargets[REG_RANK_INDEX(8,
                       gicd_reg - GICD_ITARGETSR, DABT_WORD)], *r, gicd_reg);
        vgic_unlock_rank(v, rank);
        return 1;

    case GICD_IPRIORITYR ... GICD_IPRIORITYRN:
        if ( dabt.size != DABT_BYTE && dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 8, gicd_reg - GICD_IPRIORITYR, DABT_WORD);
        if ( rank == NULL) goto write_ignore;
        vgic_lock_rank(v, rank);
        if ( dabt.size == DABT_WORD )
            rank->ipriority[REG_RANK_INDEX(8, gicd_reg - GICD_IPRIORITYR,
                                           DABT_WORD)] = *r;
        else
            vgic_byte_write(&rank->ipriority[REG_RANK_INDEX(8,
                        gicd_reg - GICD_IPRIORITYR, DABT_WORD)], *r, gicd_reg);
        vgic_unlock_rank(v, rank);
        return 1;

    case GICD_ICFGR: /* SGIs */
        goto write_ignore;
    case GICD_ICFGR + 1: /* PPIs */
        /* It is implementation defined if these are writeable. We chose not */
        goto write_ignore;
    case GICD_ICFGR + 2 ... GICD_ICFGRN: /* SPIs */
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 2, gicd_reg - GICD_ICFGR, DABT_WORD);
        if ( rank == NULL) goto write_ignore;
        vgic_lock_rank(v, rank);
        rank->icfg[REG_RANK_INDEX(2, gicd_reg - GICD_ICFGR, DABT_WORD)] = *r;
        vgic_unlock_rank(v, rank);
        return 1;

    case GICD_NSACR ... GICD_NSACRN:
        /* We do not implement security extensions for guests, write ignore */
        goto write_ignore;

    case GICD_SGIR:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        return vgic_send_sgi(v, *r);

    case GICD_CPENDSGIR ... GICD_CPENDSGIRN:
        if ( dabt.size != DABT_BYTE && dabt.size != DABT_WORD ) goto bad_width;
        printk("vGICD: unhandled %s write %#"PRIregister" to ICPENDSGIR%d\n",
               dabt.size ? "word" : "byte", *r, gicd_reg - GICD_CPENDSGIR);
        return 0;

    case GICD_SPENDSGIR ... GICD_SPENDSGIRN:
        if ( dabt.size != DABT_BYTE && dabt.size != DABT_WORD ) goto bad_width;
        printk("vGICD: unhandled %s write %#"PRIregister" to ISPENDSGIR%d\n",
               dabt.size ? "word" : "byte", *r, gicd_reg - GICD_SPENDSGIR);
        return 0;

    /* Implementation defined -- write ignored */
    case 0xfd0 ... 0xfe4:
        goto write_ignore;

    /* R/O -- write ignore */
    case GICD_ICPIDR2:
        goto write_ignore;

    /* Implementation defined -- write ignored */
    case 0xfec ... 0xffc:
        goto write_ignore;

    /* Reserved -- write ignored */
    case 0x00c ... 0x01c:
    case 0x040 ... 0x07c:
    case 0x7fc:
    case 0xbfc:
    case 0xf04 ... 0xf0c:
    case 0xf30 ... 0xfcc:
        goto write_ignore;

    default:
        printk("vGICD: unhandled write r%d=%"PRIregister" offset %#08x\n",
               dabt.reg, *r, gicd_reg);
        return 0;
    }

bad_width:
    printk("vGICD: bad write width %d r%d=%"PRIregister" offset %#08x\n",
           dabt.size, dabt.reg, *r, gicd_reg);
    domain_crash_synchronous();
    return 0;

write_ignore:
    if ( dabt.size != DABT_WORD ) goto bad_width;
    return 1;
}

const struct mmio_handler_ops vgic_v2_distr_mmio_handler = {
    .read_handler  = vgic_v2_distr_mmio_read,
    .write_handler = vgic_v2_distr_mmio_write,
};

static int vgic_v2_vcpu_init(struct vcpu *v)
{
    int i;

    /* For SGI and PPI the target is always this CPU */
    for ( i = 0 ; i < 8 ; i++ )
        v->arch.vgic.private_irqs->v2.itargets[i] =
              (1<<(v->vcpu_id+0))
            | (1<<(v->vcpu_id+8))
            | (1<<(v->vcpu_id+16))
            | (1<<(v->vcpu_id+24));

    return 0;
}

static int vgic_v2_domain_init(struct domain *d)
{
    /* We rely on gicv_setup() to initialize dbase(vGIC distributor base) */
    register_mmio_handler(d, &vgic_v2_distr_mmio_handler, d->arch.vgic.dbase,
                          PAGE_SIZE);

    return 0;
}

const static struct vgic_ops vgic_v2_ops = {
    .vcpu_init   = vgic_v2_vcpu_init,
    .domain_init = vgic_v2_domain_init,
    .send_sgi    = vgic_v2_to_sgi,
};

int vgic_v2_init(struct domain *d)
{
    register_vgic_ops(d, &vgic_v2_ops);

    return 0;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
