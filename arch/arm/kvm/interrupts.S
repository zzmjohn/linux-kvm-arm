/*
 * Copyright (C) 2012 - Virtual Open Systems and Columbia University
 * Author: Christoffer Dall <c.dall@virtualopensystems.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/linkage.h>
#include <linux/const.h>
#include <asm/unified.h>
#include <asm/page.h>
#include <asm/asm-offsets.h>
#include <asm/kvm_asm.h>
#include <asm/kvm_arm.h>
#include <asm/vfpmacros.h>

#define VCPU_USR_REG(_reg_nr)	(VCPU_USR_REGS + (_reg_nr * 4))
#define VCPU_USR_SP		(VCPU_USR_REG(13))
#define VCPU_FIQ_REG(_reg_nr)	(VCPU_FIQ_REGS + (_reg_nr * 4))
#define VCPU_FIQ_SPSR		(VCPU_FIQ_REG(7))

	.text
	.align	PAGE_SHIFT

__kvm_hyp_code_start:
	.globl __kvm_hyp_code_start

@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@  Flush per-VMID TLBs
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

/*
 * void __kvm_tlb_flush_vmid(struct kvm *kvm);
 *
 * We rely on the hardware to broadcast the TLB invalidation to all CPUs
 * inside the inner-shareable domain (which is the case for all v7
 * implementations).  If we come across a non-IS SMP implementation, we'll
 * have to use an IPI based mechanism. Until then, we stick to the simple
 * hardware assisted version.
 */
ENTRY(__kvm_tlb_flush_vmid)
	hvc	#0			@ Switch to Hyp mode
	push	{r2, r3}

	add	r0, r0, #KVM_VTTBR
	ldrd	r2, r3, [r0]
	mcrr	p15, 6, r2, r3, c2	@ Write VTTBR
	isb
	mcr     p15, 0, r0, c8, c3, 0	@ TLBIALLIS (rt ignored)
	dsb
	isb
	mov	r2, #0
	mov	r3, #0
	mcrr	p15, 6, r2, r3, c2	@ Back to VMID #0
	isb

	pop	{r2, r3}
	hvc	#0			@ Back to SVC
	bx	lr
ENDPROC(__kvm_tlb_flush_vmid)

@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@  Flush TLBs and instruction caches of current CPU for all VMIDs
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

/*
 * void __kvm_flush_vm_context(void);
 */
ENTRY(__kvm_flush_vm_context)
	hvc	#0			@ switch to hyp-mode

	mov	r0, #0			@ rn parameter for c15 flushes is SBZ
	mcr     p15, 4, r0, c8, c7, 4   @ Invalidate Non-secure Non-Hyp TLB
	mcr     p15, 0, r0, c7, c5, 0   @ Invalidate instruction caches
	dsb
	isb

	hvc	#0			@ switch back to svc-mode, see hyp_svc
	bx	lr
ENDPROC(__kvm_flush_vm_context)

/* Clobbers {r2-r6} */
.macro store_vfp_state vfp_base
	@ The VFPFMRX and VFPFMXR macros are the VMRS and VMSR instructions
	VFPFMRX	r2, FPEXC
	@ Make sure VFP is enabled so we can touch the registers.
	orr	r6, r2, #FPEXC_EN
	VFPFMXR	FPEXC, r6

	VFPFMRX	r3, FPSCR
	tst	r2, #FPEXC_EX		@ Check for VFP Subarchitecture
	beq	1f
	@ If FPEXC_EX is 0, then FPINST/FPINST2 reads are upredictable, so
	@ we only need to save them if FPEXC_EX is set.
	VFPFMRX r4, FPINST
	tst	r2, #FPEXC_FP2V
	VFPFMRX r5, FPINST2, ne		@ vmrsne
	bic	r6, r2, #FPEXC_EX	@ FPEXC_EX disable
	VFPFMXR	FPEXC, r6
1:
	VFPFSTMIA \vfp_base, r6		@ Save VFP registers
	stm	\vfp_base, {r2-r5}	@ Save FPEXC, FPSCR, FPINST, FPINST2
.endm

/* Assume FPEXC_EN is on and FPEXC_EX is off, clobbers {r2-r6} */
.macro restore_vfp_state vfp_base
	VFPFLDMIA \vfp_base, r6		@ Load VFP registers
	ldm	\vfp_base, {r2-r5}	@ Load FPEXC, FPSCR, FPINST, FPINST2

	VFPFMXR FPSCR, r3
	tst	r2, #FPEXC_EX		@ Check for VFP Subarchitecture
	beq	1f
	VFPFMXR FPINST, r4
	tst	r2, #FPEXC_FP2V
	VFPFMXR FPINST2, r5, ne
1:
	VFPFMXR FPEXC, r2	@ FPEXC	(last, in case !EN)
.endm


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@  Hypervisor world-switch code
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

/* These are simply for the macros to work - value don't have meaning */
.equ usr, 0
.equ svc, 1
.equ abt, 2
.equ und, 3
.equ irq, 4
.equ fiq, 5

.macro store_mode_state base_reg, mode
	.if \mode == usr
	mrs	r2, SP_usr
	mov	r3, lr
	stmdb	\base_reg!, {r2, r3}
	.elseif \mode != fiq
	mrs	r2, SP_\mode
	mrs	r3, LR_\mode
	mrs	r4, SPSR_\mode
	stmdb	\base_reg!, {r2, r3, r4}
	.else
	mrs	r2, r8_fiq
	mrs	r3, r9_fiq
	mrs	r4, r10_fiq
	mrs	r5, r11_fiq
	mrs	r6, r12_fiq
	mrs	r7, SP_fiq
	mrs	r8, LR_fiq
	mrs	r9, SPSR_fiq
	stmdb	\base_reg!, {r2-r9}
	.endif
.endm

.macro load_mode_state base_reg, mode
	.if \mode == usr
	ldmia	\base_reg!, {r2, r3}
	msr	SP_usr, r2
	mov	lr, r3
	.elseif \mode != fiq
	ldmia	\base_reg!, {r2, r3, r4}
	msr	SP_\mode, r2
	msr	LR_\mode, r3
	msr	SPSR_\mode, r4
	.else
	ldmia	\base_reg!, {r2-r9}
	msr	r8_fiq, r2
	msr	r9_fiq, r3
	msr	r10_fiq, r4
	msr	r11_fiq, r5
	msr	r12_fiq, r6
	msr	SP_fiq, r7
	msr	LR_fiq, r8
	msr	SPSR_fiq, r9
	.endif
.endm

/* Reads cp15 registers from hardware and stores them in memory
 * @vcpu:   If 0, registers are written in-order to the stack,
 * 	    otherwise to the VCPU struct pointed to by vcpup
 * @vcpup:  Register pointing to VCPU struct
 */
.macro read_cp15_state vcpu=0, vcpup
	mrc	p15, 0, r2, c1, c0, 0	@ SCTLR
	mrc	p15, 0, r3, c1, c0, 2	@ CPACR
	mrc	p15, 0, r4, c2, c0, 2	@ TTBCR
	mrc	p15, 0, r5, c3, c0, 0	@ DACR
	mrrc	p15, 0, r6, r7, c2	@ TTBR 0
	mrrc	p15, 1, r8, r9, c2	@ TTBR 1
	mrc	p15, 0, r10, c10, c2, 0	@ PRRR
	mrc	p15, 0, r11, c10, c2, 1	@ NMRR
	mrc	p15, 2, r12, c0, c0, 0	@ CSSELR

	.if \vcpu == 0
	push	{r2-r12}		@ Push CP15 registers
	.else
	str	r2, [\vcpup, #VCPU_SCTLR]
	str	r3, [\vcpup, #VCPU_CPACR]
	str	r4, [\vcpup, #VCPU_TTBCR]
	str	r5, [\vcpup, #VCPU_DACR]
	add	\vcpup, \vcpup, #VCPU_TTBR0
	strd	r6, r7, [\vcpup]
	add	\vcpup, \vcpup, #(VCPU_TTBR1 - VCPU_TTBR0)
	strd	r8, r9, [\vcpup]
	sub	\vcpup, \vcpup, #(VCPU_TTBR1)
	str	r10, [\vcpup, #VCPU_PRRR]
	str	r11, [\vcpup, #VCPU_NMRR]
	str	r12, [\vcpup, #VCPU_CSSELR]
	.endif

	mrc	p15, 0, r2, c13, c0, 1	@ CID
	mrc	p15, 0, r3, c13, c0, 2	@ TID_URW
	mrc	p15, 0, r4, c13, c0, 3	@ TID_URO
	mrc	p15, 0, r5, c13, c0, 4	@ TID_PRIV
	mrc	p15, 0, r6, c5, c0, 0	@ DFSR
	mrc	p15, 0, r7, c5, c0, 1	@ IFSR
	mrc	p15, 0, r8, c5, c1, 0	@ ADFSR
	mrc	p15, 0, r9, c5, c1, 1	@ AIFSR
	mrc	p15, 0, r10, c6, c0, 0	@ DFAR
	mrc	p15, 0, r11, c6, c0, 2	@ IFAR
	mrc	p15, 0, r12, c12, c0, 0	@ VBAR

	.if \vcpu == 0
	push	{r2-r12}		@ Push CP15 registers
	.else
	str	r2, [\vcpup, #VCPU_CID]
	str	r3, [\vcpup, #VCPU_TID_URW]
	str	r4, [\vcpup, #VCPU_TID_URO]
	str	r5, [\vcpup, #VCPU_TID_PRIV]
	str	r6, [\vcpup, #VCPU_DFSR]
	str	r7, [\vcpup, #VCPU_IFSR]
	str	r8, [\vcpup, #VCPU_ADFSR]
	str	r9, [\vcpup, #VCPU_AIFSR]
	str	r10, [\vcpup, #VCPU_DFAR]
	str	r11, [\vcpup, #VCPU_IFAR]
	str	r12, [\vcpup, #VCPU_VBAR]
	.endif
.endm

/* Reads cp15 registers from memory and writes them to hardware
 * @vcpu:   If 0, registers are read in-order from the stack,
 * 	    otherwise from the VCPU struct pointed to by vcpup
 * @vcpup:  Register pointing to VCPU struct
 */
.macro write_cp15_state vcpu=0, vcpup
	.if \vcpu == 0
	pop	{r2-r12}
	.else
	ldr	r2, [\vcpup, #VCPU_CID]
	ldr	r3, [\vcpup, #VCPU_TID_URW]
	ldr	r4, [\vcpup, #VCPU_TID_URO]
	ldr	r5, [\vcpup, #VCPU_TID_PRIV]
	ldr	r6, [\vcpup, #VCPU_DFSR]
	ldr	r7, [\vcpup, #VCPU_IFSR]
	ldr	r8, [\vcpup, #VCPU_ADFSR]
	ldr	r9, [\vcpup, #VCPU_AIFSR]
	ldr	r10, [\vcpup, #VCPU_DFAR]
	ldr	r11, [\vcpup, #VCPU_IFAR]
	ldr	r12, [\vcpup, #VCPU_VBAR]
	.endif

	mcr	p15, 0, r2, c13, c0, 1	@ CID
	mcr	p15, 0, r3, c13, c0, 2	@ TID_URW
	mcr	p15, 0, r4, c13, c0, 3	@ TID_URO
	mcr	p15, 0, r5, c13, c0, 4	@ TID_PRIV
	mcr	p15, 0, r6, c5, c0, 0	@ DFSR
	mcr	p15, 0, r7, c5, c0, 1	@ IFSR
	mcr	p15, 0, r8, c5, c1, 0	@ ADFSR
	mcr	p15, 0, r9, c5, c1, 1	@ AIFSR
	mcr	p15, 0, r10, c6, c0, 0	@ DFAR
	mcr	p15, 0, r11, c6, c0, 2	@ IFAR
	mcr	p15, 0, r12, c12, c0, 0	@ VBAR

	.if \vcpu == 0
	pop	{r2-r12}
	.else
	ldr	r2, [\vcpup, #VCPU_SCTLR]
	ldr	r3, [\vcpup, #VCPU_CPACR]
	ldr	r4, [\vcpup, #VCPU_TTBCR]
	ldr	r5, [\vcpup, #VCPU_DACR]
	add	\vcpup, \vcpup, #VCPU_TTBR0
	ldrd	r6, r7, [\vcpup]
	add	\vcpup, \vcpup, #(VCPU_TTBR1 - VCPU_TTBR0)
	ldrd	r8, r9, [\vcpup]
	sub	\vcpup, \vcpup, #(VCPU_TTBR1)
	ldr	r10, [\vcpup, #VCPU_PRRR]
	ldr	r11, [\vcpup, #VCPU_NMRR]
	ldr	r12, [\vcpup, #VCPU_CSSELR]
	.endif

	mcr	p15, 0, r2, c1, c0, 0	@ SCTLR
	mcr	p15, 0, r3, c1, c0, 2	@ CPACR
	mcr	p15, 0, r4, c2, c0, 2	@ TTBCR
	mcr	p15, 0, r5, c3, c0, 0	@ DACR
	mcrr	p15, 0, r6, r7, c2	@ TTBR 0
	mcrr	p15, 1, r8, r9, c2	@ TTBR 1
	mcr	p15, 0, r10, c10, c2, 0	@ PRRR
	mcr	p15, 0, r11, c10, c2, 1	@ NMRR
	mcr	p15, 2, r12, c0, c0, 0	@ CSSELR
.endm

/*
 * Save the VGIC CPU state into memory
 * @vcpup: Register pointing to VCPU struct
 */
.macro save_vgic_state	vcpup
#ifdef CONFIG_KVM_ARM_VGIC
	/* Get VGIC VCTRL base into r2 */
	ldr	r2, [\vcpup, #VCPU_KVM]
	ldr	r2, [r2, #KVM_VGIC_VCTRL]
	cmp	r2, #0
	beq	2f

	/* Compute the address of struct vgic_cpu */
	add	r11, \vcpup, #VCPU_VGIC_CPU

	/* Save all interesting registers */
	ldr	r3, [r2, #GICH_HCR]
	ldr	r4, [r2, #GICH_VMCR]
	ldr	r5, [r2, #GICH_MISR]
	ldr	r6, [r2, #GICH_EISR0]
	ldr	r7, [r2, #GICH_EISR1]
	ldr	r8, [r2, #GICH_ELRSR0]
	ldr	r9, [r2, #GICH_ELRSR1]
	ldr	r10, [r2, #GICH_APR]

	str	r3, [r11, #VGIC_CPU_HCR]
	str	r4, [r11, #VGIC_CPU_VMCR]
	str	r5, [r11, #VGIC_CPU_MISR]
	str	r6, [r11, #VGIC_CPU_EISR]
	str	r7, [r11, #(VGIC_CPU_EISR + 4)]
	str	r8, [r11, #VGIC_CPU_ELRSR]
	str	r9, [r11, #(VGIC_CPU_ELRSR + 4)]
	str	r10, [r11, #VGIC_CPU_APR]

	/* Save list registers */
	add	r2, r2, #GICH_LR0
	add	r3, r11, #VGIC_CPU_LR
	ldr	r4, [r11, #VGIC_CPU_NR_LR]
1:	ldr	r6, [r2], #4
	str	r6, [r3], #4
	subs	r4, r4, #1
	bne	1b
2:
#endif
.endm

/*
 * Restore the VGIC CPU state from memory
 * @vcpup: Register pointing to VCPU struct
 */
.macro restore_vgic_state	vcpup
#ifdef CONFIG_KVM_ARM_VGIC
	/* Get VGIC VCTRL base into r2 */
	ldr	r2, [\vcpup, #VCPU_KVM]
	ldr	r2, [r2, #KVM_VGIC_VCTRL]
	cmp	r2, #0
	beq	2f

	/* Compute the address of struct vgic_cpu */
	add	r11, \vcpup, #VCPU_VGIC_CPU

	/* We only restore a minimal set of registers */
	ldr	r3, [r11, #VGIC_CPU_HCR]
	ldr	r4, [r11, #VGIC_CPU_VMCR]
	ldr	r8, [r11, #VGIC_CPU_APR]

	str	r3, [r2, #GICH_HCR]
	str	r4, [r2, #GICH_VMCR]
	str	r8, [r2, #GICH_APR]

	/* Restore list registers */
	add	r2, r2, #GICH_LR0
	add	r3, r11, #VGIC_CPU_LR
	ldr	r4, [r11, #VGIC_CPU_NR_LR]
1:	ldr	r6, [r3], #4
	str	r6, [r2], #4
	subs	r4, r4, #1
	bne	1b
2:
#endif
.endm

#define CNTHCTL_PL1PCTEN	(1 << 0)
#define CNTHCTL_PL1PCEN		(1 << 1)

.macro save_timer_state	vcpup
#ifdef CONFIG_KVM_ARM_TIMER
	ldr	r4, [\vcpup, #VCPU_KVM]
	ldr	r2, [r4, #KVM_TIMER_ENABLED]
	cmp	r2, #0
	beq	1f

	mrc	p15, 0, r2, c14, c3, 1	@ CNTV_CTL
	and	r2, #3
	str	r2, [\vcpup, #VCPU_TIMER_CNTV_CTL]
	bic	r2, #1			@ Clear ENABLE
	mcr	p15, 0, r2, c14, c3, 1	@ CNTV_CTL
	isb

	mrrc	p15, 3, r2, r3, c14	@ CNTV_CVAL
	str	r3, [\vcpup, #VCPU_TIMER_CNTV_CVALH]
	str	r2, [\vcpup, #VCPU_TIMER_CNTV_CVALL]

1:
#endif
	@ Allow physical timer/counter access for the host
	mrc	p15, 4, r2, c14, c1, 0	@ CNTHCTL
	orr	r2, r2, #(CNTHCTL_PL1PCEN | CNTHCTL_PL1PCTEN)
	mcr	p15, 4, r2, c14, c1, 0	@ CNTHCTL
.endm

.macro restore_timer_state vcpup
	@ Disallow physical timer access for the guest
	@ Physical counter access is allowed
	mrc	p15, 4, r2, c14, c1, 0	@ CNTHCTL
	orr	r2, r2, #CNTHCTL_PL1PCTEN
	bic	r2, r2, #CNTHCTL_PL1PCEN
	mcr	p15, 4, r2, c14, c1, 0	@ CNTHCTL

#ifdef CONFIG_KVM_ARM_TIMER
	ldr	r4, [\vcpup, #VCPU_KVM]
	ldr	r2, [r4, #KVM_TIMER_ENABLED]
	cmp	r2, #0
	beq	1f

	ldr	r3, [r4, #KVM_TIMER_CNTVOFF_H]
	ldr	r2, [r4, #KVM_TIMER_CNTVOFF_L]
	mcrr	p15, 4, r2, r3, c14	@ CNTVOFF
	isb

	ldr	r3, [\vcpup, #VCPU_TIMER_CNTV_CVALH]
	ldr	r2, [\vcpup, #VCPU_TIMER_CNTV_CVALL]
	mcrr	p15, 3, r2, r3, c14	@ CNTV_CVAL

	ldr	r2, [\vcpup, #VCPU_TIMER_CNTV_CTL]
	and	r2, #3
	mcr	p15, 0, r2, c14, c3, 1	@ CNTV_CTL
	isb
1:
#endif
.endm

/* Configures the HSTR (Hyp System Trap Register) on entry/return
 * (hardware reset value is 0) */
.macro set_hstr entry
	mrc	p15, 4, r2, c1, c1, 3
	ldr	r3, =HSTR_T(15)
	.if \entry == 1
	orr	r2, r2, r3		@ Trap CR{15}
	.else
	bic	r2, r2, r3		@ Don't trap any CRx accesses
	.endif
	mcr	p15, 4, r2, c1, c1, 3
.endm

/* Configures the HCPTR (Hyp Coprocessor Trap Register) on entry/return
 * (hardware reset value is 0). Keep previous value in r2. */
.macro set_hcptr entry, mask
	mrc	p15, 4, r2, c1, c1, 2
	ldr	r3, =\mask
	.if \entry == 1
	orr	r3, r2, r3		@ Trap coproc-accesses defined in mask
	.else
	bic	r3, r2, r3		@ Don't trap defined coproc-accesses
	.endif
	mcr	p15, 4, r3, c1, c1, 2
.endm

/* Configures the HDCR (Hyp Debug Configuration Register) on entry/return
 * (hardware reset value is 0) */
.macro set_hdcr entry
	mrc	p15, 4, r2, c1, c1, 1
	ldr	r3, =(HDCR_TPM|HDCR_TPMCR)
	.if \entry == 1
	orr	r2, r2, r3		@ Trap some perfmon accesses
	.else
	bic	r2, r2, r3		@ Don't trap any perfmon accesses
	.endif
	mcr	p15, 4, r2, c1, c1, 1
.endm

/* Enable/Disable: stage-2 trans., trap interrupts, trap wfi, trap smc */
.macro configure_hyp_role entry, vcpu_ptr
	mrc	p15, 4, r2, c1, c1, 0	@ HCR
	bic	r2, r2, #HCR_VIRT_EXCP_MASK
	ldr	r3, =HCR_GUEST_MASK
	.if \entry == 1
	orr	r2, r2, r3
	ldr	r3, [\vcpu_ptr, #VCPU_IRQ_LINES]
	orr	r2, r2, r3
	.else
	bic	r2, r2, r3
	.endif
	mcr	p15, 4, r2, c1, c1, 0
.endm

.macro load_vcpu reg
	mrc	p15, 4, \reg, c13, c0, 2	@ HTPIDR
.endm

@ Arguments:
@  r0: pointer to vcpu struct
ENTRY(__kvm_vcpu_run)
	hvc	#0			@ switch to hyp-mode

	@ Save the vcpu pointer
	mcr	p15, 4, r0, c13, c0, 2	@ HTPIDR

	@ Now we're in Hyp-mode and lr_usr, spsr_hyp are on the stack
	mrs	r2, sp_usr
	push	{r2}			@ Push r13_usr
	push	{r4-r12}		@ Push r4-r12

	store_mode_state sp, svc
	store_mode_state sp, abt
	store_mode_state sp, und
	store_mode_state sp, irq
	store_mode_state sp, fiq

	restore_vgic_state r0
	restore_timer_state r0

	@ Store hardware CP15 state and load guest state
	read_cp15_state
	write_cp15_state 1, r0

	@ If the host kernel has not been configured with VFPv3 support,
	@ then it is safer if we deny guests from using it as well.
#ifdef CONFIG_VFPv3
	@ Set FPEXC_EN so the guest doesn't trap floating point instructions
	VFPFMRX r2, FPEXC		@ VMRS
	push	{r2}
	orr	r2, r2, #FPEXC_EN
	VFPFMXR FPEXC, r2		@ VMSR
#endif

	@ Configure Hyp-role
	configure_hyp_role 1, r0

	@ Trap coprocessor CRx accesses
	set_hstr 1
	set_hcptr 1, (HCPTR_TTA | HCPTR_TCP(10) | HCPTR_TCP(11))
	set_hdcr 1

	@ Write configured ID register into MIDR alias
	ldr	r1, [r0, #VCPU_MIDR]
	mcr	p15, 4, r1, c0, c0, 0

	@ Write guest view of MPIDR into VMPIDR
	ldr	r1, [r0, #VCPU_MPIDR]
	mcr	p15, 4, r1, c0, c0, 5

	@ Load guest registers
	add	r0, r0, #(VCPU_USR_SP)
	load_mode_state r0, usr
	load_mode_state r0, svc
	load_mode_state r0, abt
	load_mode_state r0, und
	load_mode_state r0, irq
	load_mode_state r0, fiq

	@ Load return state (r0 now points to vcpu->arch.regs.pc)
	ldmia	r0, {r2, r3}
	msr	ELR_hyp, r2
	msr	SPSR_cxsf, r3

	@ Set up guest memory translation
	sub	r1, r0, #(VCPU_PC - VCPU_KVM)	@ r1 points to kvm struct
	ldr	r1, [r1]
	add	r1, r1, #KVM_VTTBR
	ldrd	r2, r3, [r1]
	mcrr	p15, 6, r2, r3, c2	@ Write VTTBR

	@ Load remaining registers and do the switch
	sub	r0, r0, #(VCPU_PC - VCPU_USR_REGS)
	ldmia	r0, {r0-r12}
	clrex				@ Clear exclusive monitor
	eret

__kvm_vcpu_return:
	@ Set VMID == 0
	mov	r2, #0
	mov	r3, #0
	mcrr	p15, 6, r2, r3, c2	@ Write VTTBR

	@ Store return state
	mrs	r2, ELR_hyp
	mrs	r3, spsr
	str	r2, [r1, #VCPU_PC]
	str	r3, [r1, #VCPU_CPSR]

	@ Store guest registers
	add	r1, r1, #(VCPU_FIQ_SPSR + 4)
	store_mode_state r1, fiq
	store_mode_state r1, irq
	store_mode_state r1, und
	store_mode_state r1, abt
	store_mode_state r1, svc
	store_mode_state r1, usr
	sub	r1, r1, #(VCPU_USR_REG(13))

	@ Don't trap coprocessor accesses for host kernel
	set_hstr 0
	set_hdcr 0
	set_hcptr 0, (HCPTR_TTA | HCPTR_TCP(10) | HCPTR_TCP(11))

#ifdef CONFIG_VFPv3
	@ Save floating point registers we if let guest use them.
	tst	r2, #(HCPTR_TCP(10) | HCPTR_TCP(11))
	bne	after_vfp_restore

	@ Switch VFP/NEON hardware state to the host's
	add	r7, r1, #VCPU_VFP_GUEST
	store_vfp_state r7
	add	r7, r1, #VCPU_VFP_HOST
	ldr	r7, [r7]
	restore_vfp_state r7

after_vfp_restore:
	@ Restore FPEXC_EN which we clobbered on entry
	pop	{r2}
	VFPFMXR FPEXC, r2
#endif

	@ Reset Hyp-role
	configure_hyp_role 0, r1

	@ Let host read hardware MIDR
	mrc	p15, 0, r2, c0, c0, 0
	mcr	p15, 4, r2, c0, c0, 0

	@ Back to hardware MPIDR
	mrc	p15, 0, r2, c0, c0, 5
	mcr	p15, 4, r2, c0, c0, 5

	@ Store guest CP15 state and restore host state
	read_cp15_state 1, r1
	write_cp15_state

	save_timer_state r1
	save_vgic_state	r1

	load_mode_state sp, fiq
	load_mode_state sp, irq
	load_mode_state sp, und
	load_mode_state sp, abt
	load_mode_state sp, svc

	pop	{r4-r12}		@ Pop r4-r12
	pop	{r2}			@ Pop r13_usr
	msr	sp_usr, r2

	hvc	#0			@ switch back to svc-mode, see hyp_svc

	clrex				@ Clear exclusive monitor
	bx	lr			@ return to IOCTL

@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@  Translate VA to PA
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

@ Arguments:
@  r0: pointer to vcpu struct
@  r1: virtual address to map (rounded to page)
@  r2: 1 = P1 (read) mapping, 0 = P0 (read) mapping.
@ Returns 64 bit PAR value.
ENTRY(__kvm_va_to_pa)
	hvc	#0			@ switch to hyp-mode

	push	{r4-r12}

	@ Fold flag into r1, easier than using stack.
	cmp	r2, #0
	movne	r2, #1
	orr	r1, r1, r2

	@ This swaps too many registers, but we're in the slow path anyway.
	read_cp15_state
	write_cp15_state 1, r0

	ands	r2, r1, #1
	bic	r1, r1, r2
	mcrne	p15, 0, r1, c7, c8, 0	@ VA to PA, ATS1CPR
	mcreq	p15, 0, r1, c7, c8, 2	@ VA to PA, ATS1CUR
	isb

	@ Restore host state.
	read_cp15_state 1, r0
	write_cp15_state

	mrrc	p15, 0, r0, r1, c7	@ PAR
	pop	{r4-r12}
	hvc	#0			@ Back to SVC
	bx	lr


@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@  Hypervisor exception vector and handlers
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

/*
 * The KVM/ARM Hypervisor ABI is defined as follows:
 *
 * Entry to Hyp mode from the host kernel will happen _only_ when an HVC
 * instruction is issued since all traps are disabled when running the host
 * kernel as per the Hyp-mode initialization at boot time.
 *
 * HVC instructions cause a trap to the vector page + offset 0x18 (see hyp_hvc
 * below) when the HVC instruction is called from SVC mode (i.e. a guest or the
 * host kernel) and they cause a trap to the vector page + offset 0xc when HVC
 * instructions are called from within Hyp-mode.
 *
 * Hyp-ABI: Switching from host kernel to Hyp-mode:
 *    Switching to Hyp mode is done through a simple HVC instructions. The
 *    exception vector code will check that the HVC comes from VMID==0 and if
 *    so will store the necessary state on the Hyp stack, which will look like
 *    this (growing downwards, see the hyp_hvc handler):
 *      ...
 *      stack_page + 4: spsr (Host-SVC cpsr)
 *      stack_page    : lr_usr
 *      --------------: stack bottom
 *
 * Hyp-ABI: Switching from Hyp-mode to host kernel SVC mode:
 *    When returning from Hyp mode to SVC mode, another HVC instruction is
 *    executed from Hyp mode, which is taken in the hyp_svc handler. The
 *    bottom of the Hyp is derived from the Hyp stack pointer (only a single
 *    page aligned stack is used per CPU) and the initial SVC registers are
 *    used to restore the host state.
 *
 *
 * Note that the above is used to execute code in Hyp-mode from a host-kernel
 * point of view, and is a different concept from performing a world-switch and
 * executing guest code SVC mode (with a VMID != 0).
 */

@ Handle undef, svc, pabt, or dabt by crashing with a user notice
.macro bad_exception exception_code, panic_str
	mrrc	p15, 6, r2, r3, c2	@ Read VTTBR
	lsr	r3, r3, #16
	ands	r3, r3, #0xff

	@ COND:neq means we're probably in the guest and we can try fetching
	@ the vcpu pointer and stuff off the stack and keep our fingers crossed
	beq	99f
	mov	r0, #\exception_code
	load_vcpu	r1		@ Load VCPU pointer
	.if \exception_code == ARM_EXCEPTION_DATA_ABORT
	mrc	p15, 4, r2, c5, c2, 0	@ HSR
	mrc	p15, 4, r3, c6, c0, 0	@ HDFAR
	str	r2, [r1, #VCPU_HSR]
	str	r3, [r1, #VCPU_HDFAR]
	.endif
	.if \exception_code == ARM_EXCEPTION_PREF_ABORT
	mrc	p15, 4, r2, c5, c2, 0	@ HSR
	mrc	p15, 4, r3, c6, c0, 2	@ HIFAR
	str	r2, [r1, #VCPU_HSR]
	str	r3, [r1, #VCPU_HIFAR]
	.endif
	mrs	r2, ELR_hyp
	str	r2, [r1, #VCPU_HYP_PC]
	b	__kvm_vcpu_return

	@ We were in the host already
99:	hvc	#0	@ switch to SVC mode
	ldr	r0, \panic_str
	mrs	r1, ELR_hyp
	b	panic

.endm

	.text

	.align 5
__kvm_hyp_vector:
	.globl __kvm_hyp_vector

	@ Hyp-mode exception vector
	W(b)	hyp_reset
	W(b)	hyp_undef
	W(b)	hyp_svc
	W(b)	hyp_pabt
	W(b)	hyp_dabt
	W(b)	hyp_hvc
	W(b)	hyp_irq
	W(b)	hyp_fiq

	.align
hyp_reset:
	b	hyp_reset

	.align
hyp_undef:
	bad_exception ARM_EXCEPTION_UNDEFINED, und_die_str

	.align
hyp_svc:
	@ Can only get here if HVC or SVC is called from Hyp, mode which means
	@ we want to change mode back to SVC mode.
	push	{r12}
	mov	r12, sp
	bic	r12, r12, #0x0ff
	bic	r12, r12, #0xf00
	ldr	lr, [r12, #4]
	msr	SPSR_csxf, lr
	ldr	lr, [r12]
	pop	{r12}
	eret

	.align
hyp_pabt:
	bad_exception ARM_EXCEPTION_PREF_ABORT, pabt_die_str

	.align
hyp_dabt:
	bad_exception ARM_EXCEPTION_DATA_ABORT, dabt_die_str

	.align
hyp_hvc:
	@ Getting here is either becuase of a trap from a guest or from calling
	@ HVC from the host kernel, which means "switch to Hyp mode".
	push	{r0, r1, r2}

	@ Check syndrome register
	mrc	p15, 4, r0, c5, c2, 0	@ HSR
	lsr	r1, r0, #HSR_EC_SHIFT
#ifdef CONFIG_VFPv3
	cmp	r1, #HSR_EC_CP_0_13
	beq	switch_to_guest_vfp
#endif
	cmp	r1, #HSR_EC_HVC
	bne	guest_trap		@ Not HVC instr.

	@ Let's check if the HVC came from VMID 0 and allow simple
	@ switch to Hyp mode
	mrrc    p15, 6, r1, r2, c2
	lsr     r2, r2, #16
	and     r2, r2, #0xff
	cmp     r2, #0
	bne	guest_trap		@ Guest called HVC

	@ HVC came from host. Check if this is a request to
	@ switch HVBAR to another set of vectors (kvm_exit).
	lsl	r0, r0, #16
	lsr	r0, r0, #16
	cmp	r0, #0xff
	bne	host_switch_to_hyp	@ Not HVC #0xff

	@ We're switching away from this hypervisor, let's blow the TLBs.
	pop	{r0, r1, r2}
	mcr	p15, 4, r0, c12, c0, 0  @ HVBAR
	mcr	p15, 4, r0, c8, c7, 0   @ Flush Hyp TLB, r0 ignored
	eret

host_switch_to_hyp:
	@ Store lr_usr,spsr (svc cpsr) on bottom of stack
	mov	r1, sp
	bic	r1, r1, #0x0ff
	bic	r1, r1, #0xf00
	str	lr, [r1]
	mrs	lr, spsr
	str	lr, [r1, #4]

	pop	{r0, r1, r2}

	@ Return to caller in Hyp mode
	mrs	lr, ELR_hyp
	mov	pc, lr

guest_trap:
	load_vcpu	r1		@ Load VCPU pointer
	str	r0, [r1, #VCPU_HSR]
	add	r1, r1, #VCPU_USR_REG(3)
	stmia	r1, {r3-r12}
	sub	r1, r1, #(VCPU_USR_REG(3) - VCPU_USR_REG(0))
	pop	{r3, r4, r5}
	stmia	r1, {r3, r4, r5}
	sub	r1, r1, #VCPU_USR_REG(0)

	@ Check if we need the fault information
	lsr	r2, r0, #HSR_EC_SHIFT
	cmp	r2, #HSR_EC_IABT
	beq	2f
	cmpne	r2, #HSR_EC_DABT
	bne	1f

2:	mrc	p15, 4, r2, c6, c0, 0	@ HDFAR
	mrc	p15, 4, r3, c6, c0, 2	@ HIFAR
	mrc	p15, 4, r4, c6, c0, 4	@ HPFAR
	add	r5, r1, #VCPU_HDFAR
	stmia	r5, {r2, r3, r4}

1:	mov	r0, #ARM_EXCEPTION_HVC
	b	__kvm_vcpu_return

@ If VFPv3 support is not available, then we will not switch the VFP
@ registers; however cp10 and cp11 accesses will still trap and fallback
@ to the regular coprocessor emulation code, which currently will
@ inject an undefined exception to the guest.
#ifdef CONFIG_VFPv3
switch_to_guest_vfp:
	load_vcpu	r0		@ Load VCPU pointer
	push	{r3-r7}

	@ NEON/VFP used.  Turn on VFP access.
	set_hcptr 0, (HCPTR_TCP(10) | HCPTR_TCP(11))

	@ Switch VFP/NEON hardware state to the guest's
	add	r7, r0, #VCPU_VFP_HOST
	ldr	r7, [r7]
	store_vfp_state r7
	add	r7, r0, #VCPU_VFP_GUEST
	restore_vfp_state r7

	pop	{r3-r7}
	pop	{r0-r2}
	eret
#endif

	.align
hyp_irq:
	push	{r0}
	load_vcpu	r0		@ Load VCPU pointer
	add	r0, r0, #(VCPU_USR_REG(1))
	stmia	r0, {r1-r12}
	pop	{r0}
	load_vcpu	r1		@ Load VCPU pointer again
	str	r0, [r1, #VCPU_USR_REG(0)]

	mov	r0, #ARM_EXCEPTION_IRQ
	b	__kvm_vcpu_return

	.align
hyp_fiq:
	b	hyp_fiq

	.ltorg

und_die_str:
	.ascii	"unexpected undefined exception in Hyp mode at: %#08x"
pabt_die_str:
	.ascii	"unexpected prefetch abort in Hyp mode at: %#08x"
dabt_die_str:
	.ascii	"unexpected data abort in Hyp mode at: %#08x"

/*
 * The below lines makes sure the HYP mode code fits in a single page (the
 * assembler will bark at you if it doesn't). Please keep them together. If
 * you plan to restructure the code or increase its size over a page, you'll
 * have to fix the code in init_hyp_mode().
 */
__kvm_hyp_code_end:
	.globl	__kvm_hyp_code_end

	.org	__kvm_hyp_code_start + PAGE_SIZE
