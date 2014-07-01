/*
 * xen/arch/arm/vgic-v3.c
 *
 * ARM Virtual Generic Interrupt Controller v3 support
 * based on xen/arch/arm/vgic.c
 *
 * Vijaya Kumar K <vijaya.kumar@caviumnetworks.com>
 * Copyright (c) 2014 Cavium Inc.
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
#include <asm/gic_v3_defs.h>
#include <asm/gic.h>
#include <asm/vgic.h>

static int __vgic_v3_rdistr_rd_mmio_read(struct vcpu *v, mmio_info_t *info,
                                        uint32_t gicr_reg)
{
    struct hsr_dabt dabt = info->dabt;
    struct cpu_user_regs *regs = guest_cpu_user_regs();
    register_t *r = select_user_reg(regs, dabt.reg);
    uint64_t aff;

    switch ( gicr_reg )
    {
    case GICR_CTLR:
        /* We have not implemented LPI's, read zero */
        goto read_as_zero;
    case GICR_IIDR:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        *r = GICV3_GICR_IIDR_VAL;
        return 1;
    case GICR_TYPER:
        if ( dabt.size != DABT_DOUBLE_WORD ) goto bad_width;
        /* TBD: Update processor id in [23:8] when ITS support is added */
        aff = (MPIDR_AFFINITY_LEVEL(v->arch.vmpidr, 3) << 56 |
               MPIDR_AFFINITY_LEVEL(v->arch.vmpidr, 2) << 48 |
               MPIDR_AFFINITY_LEVEL(v->arch.vmpidr, 1) << 40 |
               MPIDR_AFFINITY_LEVEL(v->arch.vmpidr, 0) << 32);
        *r = aff;
        return 1;
    case GICR_STATUSR:
        /* Not implemented */
        goto read_as_zero;
    case GICR_WAKER:
        /* Power management is not implemented */
        goto read_as_zero;
    case GICR_SETLPIR:
        /* WO. Read as zero */
        goto read_as_zero_64;
    case GICR_CLRLPIR:
        /* WO. Read as zero */
        goto read_as_zero_64;
    case GICR_PROPBASER:
        /* LPI's not implemented */
        goto read_as_zero_64;
    case GICR_PENDBASER:
        /* LPI's not implemented */
        goto read_as_zero_64;
    case GICR_INVLPIR:
        /* WO. Read as zero */
        goto read_as_zero_64;
    case GICR_INVALLR:
        /* WO. Read as zero */
        goto read_as_zero_64;
        return 0;
    case GICR_SYNCR:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        /* RO . But when read it always returns busy bito bit[0] */
        *r = GICR_SYNCR_NOT_BUSY;
        return 1;
    case GICR_MOVLPIR:
        /* WO Read as zero */
        goto read_as_zero_64;
    case GICR_MOVALLR:
        /* WO Read as zero */
        goto read_as_zero_64;
    case GICR_PIDR0:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        *r = GICV3_GICR_PIDR0;
         return 1;
    case GICR_PIDR1:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        *r = GICV3_GICR_PIDR1;
         return 1;
    case GICR_PIDR2:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        *r = GICV3_GICR_PIDR2;
         return 1;
    case GICR_PIDR3:
        /* Manufacture/customer defined */
        goto read_as_zero;
    case GICR_PIDR4:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        *r = GICV3_GICR_PIDR4;
         return 1;
    case GICR_PIDR5 ... GICR_PIDR7:
        /* Reserved0 */
        goto read_as_zero;
    default:
        printk("vGICv3: vGICR: read r%d offset %#08x\n not found",
               dabt.reg, gicr_reg);
        return 0;
    }
bad_width:
    printk("vGICv3: vGICR: bad read width %d r%d offset %#08x\n",
           dabt.size, dabt.reg, gicr_reg);
    domain_crash_synchronous();
    return 0;

read_as_zero_64:
    if ( dabt.size != DABT_DOUBLE_WORD ) goto bad_width;
    *r = 0;
    return 1;

read_as_zero:
    if ( dabt.size != DABT_WORD ) goto bad_width;
    *r = 0;
    return 1;
}

static int __vgic_v3_rdistr_rd_mmio_write(struct vcpu *v, mmio_info_t *info,
                                    uint32_t gicr_reg)
{
    struct hsr_dabt dabt = info->dabt;
    struct cpu_user_regs *regs = guest_cpu_user_regs();
    register_t *r = select_user_reg(regs, dabt.reg);

    switch ( gicr_reg )
    {
    case GICR_CTLR:
        /* LPI's not implemented */
        goto write_ignore;
    case GICR_IIDR:
        /* RO */
        goto write_ignore;
    case GICR_TYPER:
        /* RO */
        goto write_ignore_64;
    case GICR_STATUSR:
        /* Not implemented */
        goto write_ignore;
    case GICR_WAKER:
        /* Power mgmt not implemented */
        goto write_ignore;
    case GICR_SETLPIR:
        /* LPI is not implemented */
        goto write_ignore_64;
    case GICR_CLRLPIR:
        /* LPI is not implemented */
        goto write_ignore_64;
    case GICR_PROPBASER:
        /* LPI is not implemented */
        goto write_ignore_64;
    case GICR_PENDBASER:
        /* LPI is not implemented */
        goto write_ignore_64;
    case GICR_INVLPIR:
        /* LPI is not implemented */
        goto write_ignore_64;
    case GICR_INVALLR:
        /* LPI is not implemented */
        goto write_ignore_64;
    case GICR_SYNCR:
        /* RO */
        goto write_ignore;
    case GICR_MOVLPIR:
        /* LPI is not implemented */
        goto write_ignore_64;
    case GICR_MOVALLR:
        /* LPI is not implemented */
        goto write_ignore_64;
    case GICR_PIDR7... GICR_PIDR0:
        /* RO */
        goto write_ignore;
    default:
        printk("vGICR: write r%d offset %#08x\n not found", dabt.reg, gicr_reg);
        return 0;
    }
bad_width:
    printk("vGICR: bad write width %d r%d=%"PRIregister" offset %#08x\n",
           dabt.size, dabt.reg, *r, gicr_reg);
    domain_crash_synchronous();
    return 0;

write_ignore_64:
    if ( dabt.size != DABT_DOUBLE_WORD ) goto bad_width;
    return 1;

write_ignore:
    if ( dabt.size != DABT_WORD ) goto bad_width;
    return 1;
}

static int __vgic_v3_distr_common_mmio_read(struct vcpu *v, mmio_info_t *info,
                                           uint32_t reg)
{
    struct hsr_dabt dabt = info->dabt;
    struct cpu_user_regs *regs = guest_cpu_user_regs();
    register_t *r = select_user_reg(regs, dabt.reg);
    struct vgic_irq_rank *rank;

    switch ( reg )
    {
    case GICD_IGROUPR ... GICD_IGROUPRN:
        /* We do not implement security extensions for guests, read zero */
        goto read_as_zero;
    case GICD_ISENABLER ... GICD_ISENABLERN:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, reg - GICD_ISENABLER, DABT_WORD);
        if ( rank == NULL ) goto read_as_zero;
        vgic_lock_rank(v, rank);
        *r = rank->ienable;
        vgic_unlock_rank(v, rank);
        return 1;
    case GICD_ICENABLER ... GICD_ICENABLERN:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, reg - GICD_ICENABLER, DABT_WORD);
        if ( rank == NULL) goto read_as_zero;
        vgic_lock_rank(v, rank);
        *r = rank->ienable;
        vgic_unlock_rank(v, rank);
        return 1;
    case GICD_ISPENDR ... GICD_ISPENDRN:
        if ( dabt.size != DABT_BYTE && dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, reg - GICD_ISPENDR, DABT_WORD);
        if ( rank == NULL ) goto read_as_zero;
        vgic_lock_rank(v, rank);
        *r = vgic_byte_read(rank->ipend, dabt.sign, reg);
        vgic_unlock_rank(v, rank);
        return 1;
    case GICD_ICPENDR ... GICD_ICPENDRN:
        if ( dabt.size != DABT_BYTE && dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, reg - GICD_ICPENDR, DABT_WORD);
        if ( rank == NULL) goto read_as_zero;
        vgic_lock_rank(v, rank);
        *r = vgic_byte_read(rank->ipend, dabt.sign, reg);
        vgic_unlock_rank(v, rank);
        return 1;
    case GICD_ISACTIVER ... GICD_ISACTIVERN:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, reg - GICD_ISACTIVER, DABT_WORD);
        if ( rank == NULL) goto read_as_zero;
        vgic_lock_rank(v, rank);
        *r = rank->iactive;
        vgic_unlock_rank(v, rank);
        return 1;
    case GICD_ICACTIVER ... GICD_ICACTIVERN:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, reg - GICD_ICACTIVER, DABT_WORD);
        if ( rank == NULL) goto read_as_zero;
        vgic_lock_rank(v, rank);
        *r = rank->iactive;
        vgic_unlock_rank(v, rank);
        return 1;
    case GICD_IPRIORITYR ... GICD_IPRIORITYRN:
        if ( dabt.size != DABT_BYTE && dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 8, reg - GICD_IPRIORITYR, DABT_WORD);
        if ( rank == NULL) goto read_as_zero;

        vgic_lock_rank(v, rank);
        *r = rank->ipriority[REG_RANK_INDEX(8, reg - GICD_IPRIORITYR,
                                            DABT_WORD)];
        if ( dabt.size == DABT_BYTE )
            *r = vgic_byte_read(*r, dabt.sign, reg);
        vgic_unlock_rank(v, rank);
        return 1;
    case GICD_ICFGR ... GICD_ICFGRN:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 2, reg - GICD_ICFGR, DABT_WORD);
        if ( rank == NULL) goto read_as_zero;
        vgic_lock_rank(v, rank);
        *r = rank->icfg[REG_RANK_INDEX(2, reg - GICD_ICFGR, DABT_WORD)];
        vgic_unlock_rank(v, rank);
        return 1;
    default:
        printk("GICv3: vGICD/vGICR: unhandled read r%d offset %#08x\n",
               dabt.reg, reg);
        return 0;
    }

bad_width:
    dprintk(XENLOG_ERR, "vGICv3: vGICD/vGICR: bad read width %d r%d \
            offset %#08x\n", dabt.size, dabt.reg, reg);
    domain_crash_synchronous();
    return 0;

read_as_zero:
    if ( dabt.size != DABT_WORD ) goto bad_width;
    *r = 0;
    return 1;
}

static int __vgic_v3_distr_common_mmio_write(struct vcpu *v, mmio_info_t *info,
                                            uint32_t reg)
{
    struct hsr_dabt dabt = info->dabt;
    struct cpu_user_regs *regs = guest_cpu_user_regs();
    register_t *r = select_user_reg(regs, dabt.reg);
    struct vgic_irq_rank *rank;
    uint32_t tr;

    switch ( reg )
    {
    case GICD_IGROUPR ... GICD_IGROUPRN:
        /* We do not implement security extensions for guests, write ignore */
        goto write_ignore;
    case GICD_ISENABLER ... GICD_ISENABLERN:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, reg - GICD_ISENABLER, DABT_WORD);
        if ( rank == NULL) goto write_ignore;
        vgic_lock_rank(v, rank);
        tr = rank->ienable;
        rank->ienable |= *r;
        vgic_unlock_rank(v, rank);
        /* The irq number is extracted from offset. so shift by register size */
        vgic_enable_irqs(v, (*r) & (~tr), (reg - GICD_ISENABLER) >> DABT_WORD);
        return 1;
    case GICD_ICENABLER ... GICD_ICENABLERN:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, reg - GICD_ICENABLER, DABT_WORD);
        if ( rank == NULL) goto write_ignore;
        vgic_lock_rank(v, rank);
        tr = rank->ienable;
        rank->ienable &= ~*r;
        vgic_unlock_rank(v, rank);
        /* The irq number is extracted from offset. so shift by register size */
        vgic_disable_irqs(v, (*r) & tr, (reg - GICD_ICENABLER) >> DABT_WORD);
        return 1;
    case GICD_ISPENDR ... GICD_ISPENDRN:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, reg - GICD_ISPENDR, DABT_WORD);
        if ( rank == NULL ) goto write_ignore;
        vgic_lock_rank(v, rank);
        rank->ipend = *r;
        vgic_unlock_rank(v, rank);
        return 1;
    case GICD_ICPENDR ... GICD_ICPENDRN:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, reg - GICD_ICPENDR, DABT_WORD);
        if ( rank == NULL ) goto write_ignore;
        vgic_lock_rank(v, rank);
        rank->ipend &= ~*r;
        vgic_unlock_rank(v, rank);
        return 1;
    case GICD_ISACTIVER ... GICD_ISACTIVERN:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, reg - GICD_ISACTIVER, DABT_WORD);
        if ( rank == NULL) goto write_ignore;
        vgic_lock_rank(v, rank);
        rank->iactive &= ~*r;
        vgic_unlock_rank(v, rank);
        return 1;
    case GICD_ICACTIVER ... GICD_ICACTIVERN:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, reg - GICD_ICACTIVER, DABT_WORD);
        if ( rank == NULL) goto write_ignore;
        vgic_lock_rank(v, rank);
        rank->iactive &= ~*r;
        vgic_unlock_rank(v, rank);
        return 1;
    case GICD_IPRIORITYR ... GICD_IPRIORITYRN:
        if ( dabt.size != DABT_BYTE && dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 8, reg - GICD_IPRIORITYR, DABT_WORD);
        if ( rank == NULL) goto write_ignore;
        vgic_lock_rank(v, rank);
        if ( dabt.size == DABT_WORD )
            rank->ipriority[REG_RANK_INDEX(8, reg - GICD_IPRIORITYR,
                                           DABT_WORD)] = *r;
        else
            vgic_byte_write(&rank->ipriority[REG_RANK_INDEX(8,
                       reg - GICD_IPRIORITYR, DABT_WORD)], *r, reg);
        vgic_unlock_rank(v, rank);
        return 1;
    case GICD_ICFGR: /* Restricted to configure SGIs */
        goto write_ignore;
    case GICD_ICFGR + 4 ... GICD_ICFGRN: /* PPI + SPIs */
        /* ICFGR1 for PPI's, which is implementation defined
           if ICFGR1 is programmable or not. We chose to program */
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 2, reg - GICD_ICFGR, DABT_WORD);
        if ( rank == NULL) goto write_ignore;
        vgic_lock_rank(v, rank);
        rank->icfg[REG_RANK_INDEX(2, reg - GICD_ICFGR, DABT_WORD)] = *r;
        vgic_unlock_rank(v, rank);
        return 1;
    default:
        printk("vGICv3: vGICD/vGICR: unhandled write r%d \
                =%"PRIregister" offset %#08x\n", dabt.reg, *r, reg);
        return 0;
    }

bad_width:
    dprintk(XENLOG_ERR, "vGICv3: vGICD/vGICR: bad write width %d \
            r%d=%"PRIregister" offset %#08x\n", dabt.size, dabt.reg, *r, reg);
    domain_crash_synchronous();
    return 0;

write_ignore:
    if ( dabt.size != DABT_WORD ) goto bad_width;
    return 1;
}

static int vgic_v3_rdistr_sgi_mmio_read(struct vcpu *v, mmio_info_t *info,
                                     uint32_t gicr_reg)
{
    struct hsr_dabt dabt = info->dabt;
    struct cpu_user_regs *regs = guest_cpu_user_regs();
    register_t *r = select_user_reg(regs, dabt.reg);
    struct vgic_irq_rank *rank;

    switch ( gicr_reg )
    {
    case GICR_IGRPMODR0:
        /* We do not implement security extensions for guests, read zero */
        goto read_as_zero;
    case GICR_IGROUPR0:
    case GICR_ISENABLER0:
    case GICR_ICENABLER0:
    case GICR_ISACTIVER0:
    case GICR_ICACTIVER0:
    case GICR_IPRIORITYR0...GICR_IPRIORITYR7:
    case GICR_ICFGR0... GICR_ICFGR1:
         /*
          * Above registers offset are common with GICD.
          * So handle in common with GICD handling
          */
        return __vgic_v3_distr_common_mmio_read(v, info, gicr_reg);
    case GICR_ISPENDR0:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, gicr_reg - GICR_ISPENDR0, DABT_WORD);
        if ( rank == NULL ) goto read_as_zero;
        vgic_lock_rank(v, rank);
        *r = rank->pendsgi;
        vgic_unlock_rank(v, rank);
        return 1;
    case GICR_ICPENDR0:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, gicr_reg - GICR_ICPENDR0, DABT_WORD);
        if ( rank == NULL ) goto read_as_zero;
        vgic_lock_rank(v, rank);
        *r = rank->pendsgi;
        vgic_unlock_rank(v, rank);
        return 1;
    case GICR_NSACR:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        return 1;
    default:
        printk("vGICv3: vGICR: read r%d offset %#08x\n not found",
               dabt.reg, gicr_reg);
        return 0;
    }
bad_width:
    printk("vGICv3: vGICR: bad read width %d r%d offset %#08x\n",
           dabt.size, dabt.reg, gicr_reg);
    domain_crash_synchronous();
    return 0;

read_as_zero:
    if ( dabt.size != DABT_WORD ) goto bad_width;
    *r = 0;
    return 1;
}

static int vgic_v3_rdistr_sgi_mmio_write(struct vcpu *v, mmio_info_t *info,
                                      uint32_t gicr_reg)
{
    struct hsr_dabt dabt = info->dabt;
    struct cpu_user_regs *regs = guest_cpu_user_regs();
    register_t *r = select_user_reg(regs, dabt.reg);
    struct vgic_irq_rank *rank;

    switch ( gicr_reg )
    {
    case GICR_IGRPMODR0:
        /* We do not implement security extensions for guests, write ignore */
        goto write_ignore;
    case GICR_IGROUPR0:
    case GICR_ISENABLER0:
    case GICR_ICENABLER0:
    case GICR_ISACTIVER0:
    case GICR_ICACTIVER0:
    case GICR_ICFGR1:
    case GICR_IPRIORITYR0...GICR_IPRIORITYR7:
         /*
          * Above registers offset are common with GICD.
          * So handle common with GICD handling
          */
        return __vgic_v3_distr_common_mmio_write(v, info, gicr_reg);
    case GICR_ISPENDR0:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, gicr_reg - GICR_ISACTIVER0, DABT_WORD);
        if ( rank == NULL ) goto write_ignore;
        vgic_lock_rank(v, rank);
        /* TODO: we just store the SGI pending status. Handle it properly */
        rank->pendsgi |= *r;
        vgic_unlock_rank(v, rank);
        return 1;
    case GICR_ICPENDR0:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 1, gicr_reg - GICR_ISACTIVER0, DABT_WORD);
        if ( rank == NULL ) goto write_ignore;
        vgic_lock_rank(v, rank);
        /* TODO: we just store the SGI pending status. Handle it properly */
        rank->pendsgi &= ~*r;
        vgic_unlock_rank(v, rank);
        return 1;
    case GICR_NSACR:
        /* We do not implement security extensions for guests, write ignore */
        goto write_ignore;
    default:
        printk("vGICv3: vGICR SGI: write r%d offset %#08x\n not found",
               dabt.reg, gicr_reg);
        return 0;
    }

bad_width:
    printk("vGICR SGI: bad write width %d r%d=%"PRIregister" offset %#08x\n",
           dabt.size, dabt.reg, *r, gicr_reg);
    domain_crash_synchronous();
    return 0;

write_ignore:
    if ( dabt.size != DABT_WORD ) goto bad_width;
    return 1;
}

static int vgic_v3_rdistr_mmio_read(struct vcpu *v, mmio_info_t *info)
{
    uint32_t offset;

    offset = info->gpa & (v->domain->arch.vgic.rdist_stride - 1);

    if ( offset < SZ_64K )
        return __vgic_v3_rdistr_rd_mmio_read(v, info, offset);
    else  if ( (offset >= SZ_64K) && (offset < 2 * SZ_64K) )
        return vgic_v3_rdistr_sgi_mmio_read(v, info, (offset - SZ_64K));
    else
        gdprintk(XENLOG_WARNING, "vGICv3: vGICR: unknown gpa read address \
                  %"PRIpaddr"\n", info->gpa);

    return 0;
}

static int vgic_v3_rdistr_mmio_write(struct vcpu *v, mmio_info_t *info)
{
    uint32_t offset;

    offset = info->gpa & (v->domain->arch.vgic.rdist_stride - 1);
    if ( offset < SZ_64K )
        return __vgic_v3_rdistr_rd_mmio_write(v, info, offset);
    else  if ( (offset >= SZ_64K) && (offset < 2 * SZ_64K) )
        return vgic_v3_rdistr_sgi_mmio_write(v, info, (offset - SZ_64K));
    else
        gdprintk(XENLOG_WARNING, "vGICV3: vGICR: unknown gpa write address \
                 %"PRIpaddr"\n", info->gpa);

    return 0;
}

static int vgic_v3_distr_mmio_read(struct vcpu *v, mmio_info_t *info)
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
        *r = (((v->domain->max_vcpus << 5) & GICD_TYPE_CPUS ) |
              ((v->domain->arch.vgic.nr_lines / 32) & GICD_TYPE_LINES));
        return 1;
    case GICD_STATUSR:
        /*
         *  Optional, Not implemented for now.
         *  Update to support guest debugging.
         */
        goto read_as_zero;
    case GICD_IIDR:
        if ( dabt.size != DABT_WORD ) goto bad_width;
        *r = GICV3_GICD_IIDR_VAL;
        return 1;
    case 0x020 ... 0x03c:
    case 0xc000 ... 0xffcc:
        /* Implementation defined -- read as zero */
        goto read_as_zero;
    case GICD_IGROUPR ... GICD_IGROUPRN:
    case GICD_ISENABLER ... GICD_ISENABLERN:
    case GICD_ICENABLER ... GICD_ICENABLERN:
    case GICD_ISPENDR ... GICD_ISPENDRN:
    case GICD_ICPENDR ... GICD_ICPENDRN:
    case GICD_ISACTIVER ... GICD_ISACTIVERN:
    case GICD_IPRIORITYR ... GICD_IPRIORITYRN:
    case GICD_ICFGR ... GICD_ICFGRN:
        /*
         * Above all register are common with GICR and GICD
         * Manage in common
         */
        return __vgic_v3_distr_common_mmio_read(v, info, gicd_reg);
    case GICD_IROUTER ... GICD_IROUTER31:
        /* SGI/PPI is RES0 */
        goto read_as_zero_64;
    case GICD_IROUTER32 ... GICD_IROUTERN:
        if ( dabt.size != DABT_DOUBLE_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 64, gicd_reg - GICD_IROUTER, DABT_DOUBLE_WORD);
        if ( rank == NULL) goto read_as_zero;
        vgic_lock_rank(v, rank);
        /* IROUTER is 64 bit so, to make it byte size right shift by 3.
           Here once. macro REG_RANK_INDEX will do it twice */
        *r = rank->v3.irouter[REG_RANK_INDEX(64,
                           (gicd_reg - GICD_IROUTER), DABT_DOUBLE_WORD)];
        vgic_unlock_rank(v, rank);
        return 1;
    case GICD_NSACR ... GICD_NSACRN:
        /* We do not implement security extensions for guests, read zero */
        goto read_as_zero;
    case GICD_SGIR:
        /* Read as ICH_SGIR system register with SRE set. So ignore */
        goto read_as_zero;
    case GICD_CPENDSGIR ... GICD_CPENDSGIRN:
        /* Replaced with GICR_ICPENDR0. So ignore write */
        goto read_as_zero;
    case GICD_SPENDSGIR ... GICD_SPENDSGIRN:
        /* Replaced with GICR_ISPENDR0. So ignore write */
        goto read_as_zero;
    case GICD_PIDR0:
        /* GICv3 identification value */
        if ( dabt.size != DABT_WORD ) goto bad_width;
        *r = GICV3_GICD_PIDR0;
        return 1;
    case GICD_PIDR1:
        /* GICv3 identification value */
        if ( dabt.size != DABT_WORD ) goto bad_width;
        *r = GICV3_GICD_PIDR1;
        return 1;
    case GICD_PIDR2:
        /* GICv3 identification value */
        if ( dabt.size != DABT_WORD ) goto bad_width;
        *r = GICV3_GICD_PIDR2;
        return 1;
    case GICD_PIDR3:
        /* GICv3 identification value. Manufacturer/Customer defined */
        goto read_as_zero;
    case GICD_PIDR4:
        /* GICv3 identification value */
        if ( dabt.size != DABT_WORD ) goto bad_width;
        *r = GICV3_GICD_PIDR4;
        return 1;
    case GICD_PIDR5 ... GICD_PIDR7:
        /* Reserved0 */
        goto read_as_zero;
    case 0x00c:
    case 0x044:
    case 0x04c:
    case 0x05c ... 0x07c:
    case 0xf30 ... 0x5fcc:
    case 0x8000 ... 0xbfcc:
        /* These are reserved register addresses */
        printk("vGICv3: vGICD: read unknown 0x00c .. 0xfcc r%d offset %#08x\n",
               dabt.reg, gicd_reg);
        goto read_as_zero;
    default:
        printk("vGICv3: vGICD: unhandled read r%d offset %#08x\n",
               dabt.reg, gicd_reg);
        return 0;
    }

bad_width:
    dprintk(XENLOG_ERR, "vGICv3: vGICD: bad read width %d r%d offset %#08x\n",
           dabt.size, dabt.reg, gicd_reg);
    domain_crash_synchronous();
    return 0;

read_as_zero_64:
    if ( dabt.size != DABT_DOUBLE_WORD ) goto bad_width;
    *r = 0;
    return 1;

read_as_zero:
    if ( dabt.size != DABT_WORD ) goto bad_width;
    *r = 0;
    return 1;
}

static int vgic_v3_distr_mmio_write(struct vcpu *v, mmio_info_t *info)
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
        /* Ignore all but the enable bit */
        v->domain->arch.vgic.ctlr = (*r) & GICD_CTL_ENABLE;
        return 1;
    case GICD_TYPER:
        /* RO -- write ignored */
        goto write_ignore;
    case GICD_IIDR:
        /* RO -- write ignored */
        goto write_ignore;
    case GICD_STATUSR:
        /* RO -- write ignored */
        goto write_ignore;
    case GICD_SETSPI_NSR:
        /* Message based SPI is not implemented */
        goto write_ignore;
    case GICD_CLRSPI_NSR:
        /* Message based SPI is not implemented */
        goto write_ignore;
    case GICD_SETSPI_SR:
        /* Message based SPI is not implemented */
        goto write_ignore;
    case GICD_CLRSPI_SR:
        /* Message based SPI is not implemented */
        goto write_ignore;
    case 0x020 ... 0x03c:
    case 0xc000 ... 0xffcc:
        /* Implementation defined -- write ignored */
        printk("vGICD: write unknown 0x020 - 0x03c r%d offset %#08x\n",
               dabt.reg, gicd_reg);
        goto write_ignore;
    case GICD_IGROUPR ... GICD_IGROUPRN:
    case GICD_ISENABLER ... GICD_ISENABLERN:
    case GICD_ICENABLER ... GICD_ICENABLERN:
    case GICD_ISPENDR ... GICD_ISPENDRN:
    case GICD_ICPENDR ... GICD_ICPENDRN:
    case GICD_ISACTIVER ... GICD_ISACTIVERN:
    case GICD_ICACTIVER ... GICD_ICACTIVERN:
    case GICD_IPRIORITYR ... GICD_IPRIORITYRN:
    case GICD_ICFGR ... GICD_ICFGRN:
        /* Above registers are common with GICR and GICD
         * Manage in common */
        return __vgic_v3_distr_common_mmio_write(v, info, gicd_reg);
    case GICD_IROUTER ... GICD_IROUTER31:
        /* SGI/PPI is RES0 */
        goto write_ignore_64;
    case GICD_IROUTER32 ... GICD_IROUTERN:
        if ( dabt.size != DABT_DOUBLE_WORD ) goto bad_width;
        rank = vgic_rank_offset(v, 64, gicd_reg - GICD_IROUTER, DABT_DOUBLE_WORD);
        if ( rank == NULL) goto write_ignore_64;
        if ( *r )
        {
            /* TODO: Ignored. We don't support irq delivery for vcpu != 0 */
            gdprintk(XENLOG_DEBUG,
                     "SPI delivery to secondary cpus not supported\n");
            goto write_ignore_64;
        }
        vgic_lock_rank(v, rank);
        rank->v3.irouter[REG_RANK_INDEX(64,
                      (gicd_reg - GICD_IROUTER), DABT_DOUBLE_WORD)] = *r;
        vgic_unlock_rank(v, rank);
        return 1;
    case GICD_NSACR ... GICD_NSACRN:
        /* We do not implement security extensions for guests, write ignore */
        goto write_ignore;
    case GICD_SGIR:
        /* it is accessed as system register in GICv3 */
        goto write_ignore;
    case GICD_CPENDSGIR ... GICD_CPENDSGIRN:
        /* Replaced with GICR_ICPENDR0. So ignore write */
        if ( dabt.size != DABT_WORD ) goto bad_width;
        return 0;
    case GICD_SPENDSGIR ... GICD_SPENDSGIRN:
        /* Replaced with GICR_ISPENDR0. So ignore write */
        if ( dabt.size != DABT_WORD ) goto bad_width;
        return 0;
    case GICD_PIDR7... GICD_PIDR0:
        /* RO -- write ignore */
        goto write_ignore;
    case 0x00c:
    case 0x044:
    case 0x04c:
    case 0x05c ... 0x07c:
    case 0xf30 ... 0x5fcc:
    case 0x8000 ... 0xbfcc:
        /* Reserved register addresses */
        printk("vGICv3: vGICD: write unknown 0x00c 0xfcc  r%d offset %#08x\n",
                dabt.reg, gicd_reg);
        goto write_ignore;
    default:
        printk("vGICv3: vGICD: unhandled write r%d=%"PRIregister" \
                 offset %#08x\n", dabt.reg, *r, gicd_reg);
        return 0;
    }

bad_width:
    dprintk(XENLOG_ERR, "VGICv3: vGICD: bad write width %d r%d=%"PRIregister" \
             offset %#08x\n", dabt.size, dabt.reg, *r, gicd_reg);
    domain_crash_synchronous();
    return 0;

write_ignore:
    if ( dabt.size != DABT_WORD ) goto bad_width;
    return 1;

write_ignore_64:
    if ( dabt.size != DABT_DOUBLE_WORD ) goto bad_width;
    return 1;
}

static const struct mmio_handler_ops vgic_rdistr_mmio_handler = {
    .read_handler  = vgic_v3_rdistr_mmio_read,
    .write_handler = vgic_v3_rdistr_mmio_write,
};

static const struct mmio_handler_ops vgic_distr_mmio_handler = {
    .read_handler  = vgic_v3_distr_mmio_read,
    .write_handler = vgic_v3_distr_mmio_write,
};

static int vgicv3_vcpu_init(struct vcpu *v)
{
    int i;
    uint64_t affinity;

    /* For SGI and PPI the target is always this CPU */
    affinity = (MPIDR_AFFINITY_LEVEL(v->arch.vmpidr, 3) << 32 |
                MPIDR_AFFINITY_LEVEL(v->arch.vmpidr, 2) << 16 |
                MPIDR_AFFINITY_LEVEL(v->arch.vmpidr, 1) << 8  |
                MPIDR_AFFINITY_LEVEL(v->arch.vmpidr, 0));

    for ( i = 0 ; i < 32 ; i++ )
        v->arch.vgic.private_irqs->v3.irouter[i] = affinity;

    return 0;
}

static int vgicv3_domain_init(struct domain *d)
{
    int i;

    register_mmio_handler(d, &vgic_distr_mmio_handler, d->arch.vgic.dbase,
                          d->arch.vgic.dbase_size);

    /*
     * Register mmio handler per redistributor region but not for
     * every sgi rdist region which is per core.
     * The redistributor region encompasses per core sgi region.
     */
    for ( i = 0; i < d->arch.vgic.rdist_count; i++ )
        register_mmio_handler(d, &vgic_rdistr_mmio_handler,
            d->arch.vgic.rbase[i], d->arch.vgic.rbase_size[i]);

    return 0;
}

static const struct vgic_ops v3_ops = {
    .vcpu_init   = vgicv3_vcpu_init,
    .domain_init = vgicv3_domain_init,
};

int vgic_v3_init(struct domain *d)
{
    register_vgic_ops(d, &v3_ops);
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
