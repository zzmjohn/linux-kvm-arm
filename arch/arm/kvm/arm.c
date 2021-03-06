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

#include <linux/errno.h>
#include <linux/err.h>
#include <linux/kvm_host.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <linux/mman.h>
#include <linux/sched.h>
#include <linux/kvm.h>
#include <trace/events/kvm.h>

#define CREATE_TRACE_POINTS
#include "trace.h"

#include <asm/unified.h>
#include <asm/uaccess.h>
#include <asm/ptrace.h>
#include <asm/mman.h>
#include <asm/cputype.h>
#include <asm/idmap.h>
#include <asm/tlbflush.h>
#include <asm/cacheflush.h>
#include <asm/virt.h>
#include <asm/kvm_arm.h>
#include <asm/kvm_asm.h>
#include <asm/kvm_mmu.h>
#include <asm/kvm_emulate.h>
#include <asm/kvm_coproc.h>
#include <asm/opcodes.h>

#ifdef REQUIRES_VIRT
__asm__(".arch_extension	virt");
#endif

static DEFINE_PER_CPU(unsigned long, kvm_arm_hyp_stack_page);
static struct vfp_hard_struct __percpu *kvm_host_vfp_state;
static unsigned long hyp_default_vectors;

/* Per-CPU variable containing the currently running vcpu. */
static DEFINE_PER_CPU(struct kvm_vcpu *, kvm_arm_running_vcpu);

/* The VMID used in the VTTBR */
static atomic64_t kvm_vmid_gen = ATOMIC64_INIT(1);
static u8 kvm_next_vmid;
static DEFINE_SPINLOCK(kvm_vmid_lock);

static bool vgic_present;

static void kvm_arm_set_running_vcpu(struct kvm_vcpu *vcpu)
{
	BUG_ON(preemptible());
	__get_cpu_var(kvm_arm_running_vcpu) = vcpu;
}

/**
 * kvm_arm_get_running_vcpu - get the vcpu running on the current CPU.
 * Must be called from non-preemptible context
 */
struct kvm_vcpu *kvm_arm_get_running_vcpu(void)
{
	BUG_ON(preemptible());
	return __get_cpu_var(kvm_arm_running_vcpu);
}

/**
 * kvm_arm_get_running_vcpus - get the per-CPU array on currently running vcpus.
 */
struct kvm_vcpu __percpu **kvm_get_running_vcpus(void)
{
	return &kvm_arm_running_vcpu;
}

int kvm_arch_hardware_enable(void *garbage)
{
	return 0;
}

int kvm_arch_vcpu_should_kick(struct kvm_vcpu *vcpu)
{
	if (kvm_vcpu_exiting_guest_mode(vcpu) == IN_GUEST_MODE) {
		if (vgic_active_irq(vcpu) &&
		    cmpxchg(&vcpu->mode, EXITING_GUEST_MODE, IN_GUEST_MODE) == EXITING_GUEST_MODE)
			return 0;

		return 1;
	}

	return 0;
}

void kvm_arch_hardware_disable(void *garbage)
{
}

int kvm_arch_hardware_setup(void)
{
	return 0;
}

void kvm_arch_hardware_unsetup(void)
{
}

void kvm_arch_check_processor_compat(void *rtn)
{
	*(int *)rtn = 0;
}

void kvm_arch_sync_events(struct kvm *kvm)
{
}

/**
 * kvm_arch_init_vm - initializes a VM data structure
 * @kvm:	pointer to the KVM struct
 */
int kvm_arch_init_vm(struct kvm *kvm, unsigned long type)
{
	int ret = 0;

	if (type)
		return -EINVAL;

	ret = kvm_alloc_stage2_pgd(kvm);
	if (ret)
		goto out_fail_alloc;
	spin_lock_init(&kvm->arch.pgd_lock);

	ret = create_hyp_mappings(kvm, kvm + 1);
	if (ret)
		goto out_free_stage2_pgd;

	/* Mark the initial VMID generation invalid */
	kvm->arch.vmid_gen = 0;

	return ret;
out_free_stage2_pgd:
	kvm_free_stage2_pgd(kvm);
out_fail_alloc:
	return ret;
}

int kvm_arch_vcpu_fault(struct kvm_vcpu *vcpu, struct vm_fault *vmf)
{
	return VM_FAULT_SIGBUS;
}

void kvm_arch_free_memslot(struct kvm_memory_slot *free,
			   struct kvm_memory_slot *dont)
{
}

int kvm_arch_create_memslot(struct kvm_memory_slot *slot, unsigned long npages)
{
	return 0;
}

/**
 * kvm_arch_destroy_vm - destroy the VM data structure
 * @kvm:	pointer to the KVM struct
 */
void kvm_arch_destroy_vm(struct kvm *kvm)
{
	int i;

	kvm_free_stage2_pgd(kvm);

	for (i = 0; i < KVM_MAX_VCPUS; ++i) {
		if (kvm->vcpus[i]) {
			kvm_arch_vcpu_free(kvm->vcpus[i]);
			kvm->vcpus[i] = NULL;
		}
	}
}

int kvm_dev_ioctl_check_extension(long ext)
{
	int r;
	switch (ext) {
#ifdef CONFIG_KVM_ARM_VGIC
	case KVM_CAP_IRQCHIP:
		r = vgic_present;
		break;
#endif
	case KVM_CAP_USER_MEMORY:
	case KVM_CAP_DESTROY_MEMORY_REGION_WORKS:
	case KVM_CAP_ONE_REG:
		r = 1;
		break;
	case KVM_CAP_COALESCED_MMIO:
		r = KVM_COALESCED_MMIO_PAGE_OFFSET;
		break;
	default:
		r = 0;
		break;
	}
	return r;
}

long kvm_arch_dev_ioctl(struct file *filp,
			unsigned int ioctl, unsigned long arg)
{
	return -EINVAL;
}

int kvm_arch_set_memory_region(struct kvm *kvm,
			       struct kvm_userspace_memory_region *mem,
			       struct kvm_memory_slot old,
			       int user_alloc)
{
	return 0;
}

int kvm_arch_prepare_memory_region(struct kvm *kvm,
				   struct kvm_memory_slot *memslot,
				   struct kvm_memory_slot old,
				   struct kvm_userspace_memory_region *mem,
				   int user_alloc)
{
	return 0;
}

void kvm_arch_commit_memory_region(struct kvm *kvm,
				   struct kvm_userspace_memory_region *mem,
				   struct kvm_memory_slot old,
				   int user_alloc)
{
}

void kvm_arch_flush_shadow_all(struct kvm *kvm)
{
}

void kvm_arch_flush_shadow_memslot(struct kvm *kvm,
				   struct kvm_memory_slot *slot)
{
}

struct kvm_vcpu *kvm_arch_vcpu_create(struct kvm *kvm, unsigned int id)
{
	int err;
	struct kvm_vcpu *vcpu;

	vcpu = kmem_cache_zalloc(kvm_vcpu_cache, GFP_KERNEL);
	if (!vcpu) {
		err = -ENOMEM;
		goto out;
	}

	err = kvm_vcpu_init(vcpu, kvm, id);
	if (err)
		goto free_vcpu;

	err = create_hyp_mappings(vcpu, vcpu + 1);
	if (err)
		goto vcpu_uninit;

	return vcpu;
vcpu_uninit:
	kvm_vcpu_uninit(vcpu);
free_vcpu:
	kmem_cache_free(kvm_vcpu_cache, vcpu);
out:
	return ERR_PTR(err);
}

void kvm_arch_vcpu_free(struct kvm_vcpu *vcpu)
{
	kvm_mmu_free_memory_caches(vcpu);
	kvm_timer_vcpu_terminate(vcpu);
	kmem_cache_free(kvm_vcpu_cache, vcpu);
}

void kvm_arch_vcpu_destroy(struct kvm_vcpu *vcpu)
{
	kvm_arch_vcpu_free(vcpu);
}

int kvm_cpu_has_pending_timer(struct kvm_vcpu *vcpu)
{
	return 0;
}

int __attribute_const__ kvm_target_cpu(void)
{
	unsigned int midr;

	midr = read_cpuid_id();
	switch ((midr >> 4) & 0xfff) {
	case KVM_ARM_TARGET_CORTEX_A15:
		return KVM_ARM_TARGET_CORTEX_A15;
	default:
		return -EINVAL;
	}
}

int kvm_arch_vcpu_init(struct kvm_vcpu *vcpu)
{
	/* Set up VGIC */
	kvm_vgic_vcpu_init(vcpu);

	/* Set up the timer */
	kvm_timer_vcpu_init(vcpu);

	return 0;
}

void kvm_arch_vcpu_uninit(struct kvm_vcpu *vcpu)
{
}

void kvm_arch_vcpu_load(struct kvm_vcpu *vcpu, int cpu)
{
	vcpu->cpu = cpu;
	vcpu->arch.vfp_host = this_cpu_ptr(kvm_host_vfp_state);

	/*
	 * Check whether this vcpu requires the cache to be flushed on
	 * this physical CPU. This is a consequence of doing dcache
	 * operations by set/way on this vcpu. We do it here to be in
	 * a non-preemptible section.
	 */
	if (cpumask_test_cpu(cpu, &vcpu->arch.require_dcache_flush)) {
		cpumask_clear_cpu(cpu, &vcpu->arch.require_dcache_flush);
		flush_cache_all(); /* We'd really want v7_flush_dcache_all() */
	}

	kvm_arm_set_running_vcpu(vcpu);
}

void kvm_arch_vcpu_put(struct kvm_vcpu *vcpu)
{
	kvm_arm_set_running_vcpu(NULL);
}

int kvm_arch_vcpu_ioctl_set_guest_debug(struct kvm_vcpu *vcpu,
					struct kvm_guest_debug *dbg)
{
	return -EINVAL;
}


int kvm_arch_vcpu_ioctl_get_mpstate(struct kvm_vcpu *vcpu,
				    struct kvm_mp_state *mp_state)
{
	return -EINVAL;
}

int kvm_arch_vcpu_ioctl_set_mpstate(struct kvm_vcpu *vcpu,
				    struct kvm_mp_state *mp_state)
{
	return -EINVAL;
}

/**
 * kvm_arch_vcpu_runnable - determine if the vcpu can be scheduled
 * @v:		The VCPU pointer
 *
 * If the guest CPU is not waiting for interrupts or an interrupt line is
 * asserted, the CPU is by definition runnable.
 */
int kvm_arch_vcpu_runnable(struct kvm_vcpu *v)
{
	return !!v->arch.irq_lines || kvm_vgic_vcpu_pending_irq(v);
}

int kvm_arch_vcpu_in_guest_mode(struct kvm_vcpu *v)
{
	return v->mode == IN_GUEST_MODE;
}

static void reset_vm_context(void *info)
{
	__kvm_flush_vm_context();
}

/**
 * need_new_vmid_gen - check that the VMID is still valid
 * @kvm: The VM's VMID to checkt
 *
 * return true if there is a new generation of VMIDs being used
 *
 * The hardware supports only 256 values with the value zero reserved for the
 * host, so we check if an assigned value belongs to a previous generation,
 * which which requires us to assign a new value. If we're the first to use a
 * VMID for the new generation, we must flush necessary caches and TLBs on all
 * CPUs.
 */
static bool need_new_vmid_gen(struct kvm *kvm)
{
	return unlikely(kvm->arch.vmid_gen != atomic64_read(&kvm_vmid_gen));
}

/**
 * update_vttbr - Update the VTTBR with a valid VMID before the guest runs
 * @kvm	The guest that we are about to run
 *
 * Called from kvm_arch_vcpu_ioctl_run before entering the guest to ensure the
 * VM has a valid VMID, otherwise assigns a new one and flushes corresponding
 * caches and TLBs.
 */
static void update_vttbr(struct kvm *kvm)
{
	phys_addr_t pgd_phys;

	if (!need_new_vmid_gen(kvm))
		return;

	spin_lock(&kvm_vmid_lock);

	/* First user of a new VMID generation? */
	if (unlikely(kvm_next_vmid == 0)) {
		atomic64_inc(&kvm_vmid_gen);
		kvm_next_vmid = 1;

		/*
		 * On SMP we know no other CPUs can use this CPU's or
		 * each other's VMID since the kvm_vmid_lock blocks
		 * them from reentry to the guest.
		 */
		on_each_cpu(reset_vm_context, NULL, 1);
	}

	kvm->arch.vmid_gen = atomic64_read(&kvm_vmid_gen);
	kvm->arch.vmid = kvm_next_vmid;
	kvm_next_vmid++;

	/* update vttbr to be used with the new vmid */
	pgd_phys = virt_to_phys(kvm->arch.pgd);
	kvm->arch.vttbr = pgd_phys & ((1LLU << 40) - 1)
			  & ~((2 << VTTBR_X) - 1);
	kvm->arch.vttbr |= (u64)(kvm->arch.vmid) << 48;

	spin_unlock(&kvm_vmid_lock);
}

static int handle_svc_hyp(struct kvm_vcpu *vcpu, struct kvm_run *run)
{
	/* SVC called from Hyp mode should never get here */
	kvm_debug("SVC called from Hyp mode shouldn't go here\n");
	BUG();
	return -EINVAL; /* Squash warning */
}

static int handle_hvc(struct kvm_vcpu *vcpu, struct kvm_run *run)
{
	/*
	 * Guest called HVC instruction:
	 * Let it know we don't want that by injecting an undefined exception.
	 */
	kvm_debug("hvc: %x (at %08x)", vcpu->arch.hsr & ((1 << 16) - 1),
				     vcpu->arch.regs.pc);
	kvm_debug("         HSR: %8x", vcpu->arch.hsr);
	kvm_inject_undefined(vcpu);
	return 1;
}

static int handle_smc(struct kvm_vcpu *vcpu, struct kvm_run *run)
{
	/* We don't support SMC; don't do that. */
	kvm_debug("smc: at %08x", vcpu->arch.regs.pc);
	kvm_inject_undefined(vcpu);
	return 1;
}

static int handle_pabt_hyp(struct kvm_vcpu *vcpu, struct kvm_run *run)
{
	/* The hypervisor should never cause aborts */
	kvm_err("Prefetch Abort taken from Hyp mode at %#08x (HSR: %#08x)\n",
		vcpu->arch.hifar, vcpu->arch.hsr);
	return -EFAULT;
}

static int handle_dabt_hyp(struct kvm_vcpu *vcpu, struct kvm_run *run)
{
	/* This is either an error in the ws. code or an external abort */
	kvm_err("Data Abort taken from Hyp mode at %#08x (HSR: %#08x)\n",
		vcpu->arch.hdfar, vcpu->arch.hsr);
	return -EFAULT;
}

typedef int (*exit_handle_fn)(struct kvm_vcpu *, struct kvm_run *);
static exit_handle_fn arm_exit_handlers[] = {
	[HSR_EC_WFI]		= kvm_handle_wfi,
	[HSR_EC_CP15_32]	= kvm_handle_cp15_32,
	[HSR_EC_CP15_64]	= kvm_handle_cp15_64,
	[HSR_EC_CP14_MR]	= kvm_handle_cp14_access,
	[HSR_EC_CP14_LS]	= kvm_handle_cp14_load_store,
	[HSR_EC_CP14_64]	= kvm_handle_cp14_access,
	[HSR_EC_CP_0_13]	= kvm_handle_cp_0_13_access,
	[HSR_EC_CP10_ID]	= kvm_handle_cp10_id,
	[HSR_EC_SVC_HYP]	= handle_svc_hyp,
	[HSR_EC_HVC]		= handle_hvc,
	[HSR_EC_SMC]		= handle_smc,
	[HSR_EC_IABT]		= kvm_handle_guest_abort,
	[HSR_EC_IABT_HYP]	= handle_pabt_hyp,
	[HSR_EC_DABT]		= kvm_handle_guest_abort,
	[HSR_EC_DABT_HYP]	= handle_dabt_hyp,
};

/*
 * A conditional instruction is allowed to trap, even though it
 * wouldn't be executed.  So let's re-implement the hardware, in
 * software!
 */
static bool kvm_condition_valid(struct kvm_vcpu *vcpu)
{
	unsigned long cpsr, cond, insn;

	/*
	 * Exception Code 0 can only happen if we set HCR.TGE to 1, to
	 * catch undefined instructions, and then we won't get past
	 * the arm_exit_handlers test anyway.
	 */
	BUG_ON(((vcpu->arch.hsr & HSR_EC) >> HSR_EC_SHIFT) == 0);

	/* Top two bits non-zero?  Unconditional. */
	if (vcpu->arch.hsr >> 30)
		return true;

	cpsr = *vcpu_cpsr(vcpu);

	/* Is condition field valid? */
	if ((vcpu->arch.hsr & HSR_CV) >> HSR_CV_SHIFT)
		cond = (vcpu->arch.hsr & HSR_COND) >> HSR_COND_SHIFT;
	else {
		/* This can happen in Thumb mode: examine IT state. */
		unsigned long it;

		it = ((cpsr >> 8) & 0xFC) | ((cpsr >> 25) & 0x3);

		/* it == 0 => unconditional. */
		if (it == 0)
			return true;

		/* The cond for this insn works out as the top 4 bits. */
		cond = (it >> 4);
	}

	/* Shift makes it look like an ARM-mode instruction */
	insn = cond << 28;
	return arm_check_condition(insn, cpsr) != ARM_OPCODE_CONDTEST_FAIL;
}

/*
 * Return > 0 to return to guest, < 0 on error, 0 (and set exit_reason) on
 * proper exit to QEMU.
 */
static int handle_exit(struct kvm_vcpu *vcpu, struct kvm_run *run,
		       int exception_index)
{
	unsigned long hsr_ec;

	switch (exception_index) {
	case ARM_EXCEPTION_IRQ:
		return 1;
	case ARM_EXCEPTION_UNDEFINED:
		kvm_err("Undefined exception in Hyp mode at: %#08x\n",
			vcpu->arch.hyp_pc);
		BUG();
		panic("KVM: Hypervisor undefined exception!\n");
	case ARM_EXCEPTION_DATA_ABORT:
	case ARM_EXCEPTION_PREF_ABORT:
	case ARM_EXCEPTION_HVC:
		hsr_ec = (vcpu->arch.hsr & HSR_EC) >> HSR_EC_SHIFT;

		if (hsr_ec >= ARRAY_SIZE(arm_exit_handlers)
		    || !arm_exit_handlers[hsr_ec]) {
			kvm_err("Unkown exception class: %#08lx, "
				"hsr: %#08x\n", hsr_ec,
				(unsigned int)vcpu->arch.hsr);
			BUG();
		}

		/*
		 * See ARM ARM B1.14.1: "Hyp traps on instructions
		 * that fail their condition code check"
		 */
		if (!kvm_condition_valid(vcpu)) {
			bool is_wide = vcpu->arch.hsr & HSR_IL;
			kvm_skip_instr(vcpu, is_wide);
			return 1;
		}

		return arm_exit_handlers[hsr_ec](vcpu, run);
	default:
		kvm_pr_unimpl("Unsupported exception type: %d",
			      exception_index);
		run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
		return 0;
	}
}

/**
 * kvm_arch_vcpu_ioctl_run - the main VCPU run function to execute guest code
 * @vcpu:	The VCPU pointer
 * @run:	The kvm_run structure pointer used for userspace state exchange
 *
 * This function is called through the VCPU_RUN ioctl called from user space. It
 * will execute VM code in a loop until the time slice for the process is used
 * or some emulation is needed from user space in which case the function will
 * return with return value 0 and with the kvm_run structure filled in with the
 * required data for the requested emulation.
 */
int kvm_arch_vcpu_ioctl_run(struct kvm_vcpu *vcpu, struct kvm_run *run)
{
	int ret;
	sigset_t sigsaved;

	/* Make sure they initialize the vcpu with KVM_ARM_VCPU_INIT */
	if (unlikely(!vcpu->arch.target))
		return -ENOEXEC;

	if (run->exit_reason == KVM_EXIT_MMIO) {
		ret = kvm_handle_mmio_return(vcpu, vcpu->run);
		if (ret)
			return ret;
	}

	if (vcpu->sigset_active)
		sigprocmask(SIG_SETMASK, &vcpu->sigset, &sigsaved);

	ret = 1;
	run->exit_reason = KVM_EXIT_UNKNOWN;
	while (ret > 0) {
		/*
		 * Check conditions before entering the guest
		 */
		cond_resched();
		update_vttbr(vcpu->kvm);

		kvm_vgic_sync_to_cpu(vcpu);
		kvm_timer_sync_to_cpu(vcpu);

		local_irq_disable();

		/*
		 * Re-check atomic conditions
		 */
		if (signal_pending(current)) {
			ret = -EINTR;
			run->exit_reason = KVM_EXIT_INTR;
		}

		if (ret <= 0 || need_new_vmid_gen(vcpu->kvm)) {
			local_irq_enable();
			kvm_timer_sync_from_cpu(vcpu);
			kvm_vgic_sync_from_cpu(vcpu);
			continue;
		}

		BUG_ON(__vcpu_mode(*vcpu_cpsr(vcpu)) == 0xf);

		/**************************************************************
		 * Enter the guest
		 */
		trace_kvm_entry(vcpu->arch.regs.pc);
		kvm_guest_enter();
		vcpu->mode = IN_GUEST_MODE;

		smp_mb(); /* set mode before reading vcpu->arch.pause */
		if (unlikely(vcpu->arch.pause)) {
			/* This means ignore, try again. */
			ret = ARM_EXCEPTION_IRQ;
		} else {
			ret = __kvm_vcpu_run(vcpu);
		}

		vcpu->mode = OUTSIDE_GUEST_MODE;
		vcpu->arch.last_pcpu = smp_processor_id();
		kvm_guest_exit();
		trace_kvm_exit(vcpu->arch.regs.pc);
		/*
		 * We may have taken a host interrupt in HYP mode (ie
		 * while executing the guest). This interrupt is still
		 * pending, as we haven't serviced it yet!
		 *
		 * We're now back in SVC mode, with interrupts
		 * disabled.  Enabling the interrupts now will have
		 * the effect of taking the interrupt again, in SVC
		 * mode this time.
		 */
		local_irq_enable();

		/*
		 * Back from guest
		 *************************************************************/

		kvm_timer_sync_from_cpu(vcpu);
		kvm_vgic_sync_from_cpu(vcpu);

		ret = handle_exit(vcpu, run, ret);
	}

	if (vcpu->sigset_active)
		sigprocmask(SIG_SETMASK, &sigsaved, NULL);
	return ret;
}

static int vcpu_interrupt_line(struct kvm_vcpu *vcpu, int number, bool level)
{
	int bit_index;
	bool set;
	unsigned long *ptr;

	if (number == KVM_ARM_IRQ_CPU_IRQ)
		bit_index = ffs(HCR_VI) - 1;
	else /* KVM_ARM_IRQ_CPU_FIQ */
		bit_index = ffs(HCR_VF) - 1;

	ptr = (unsigned long *)&vcpu->arch.irq_lines;
	if (level)
		set = test_and_set_bit(bit_index, ptr);
	else
		set = test_and_clear_bit(bit_index, ptr);

	/*
	 * If we didn't change anything, no need to wake up or kick other CPUs
	 */
	if (set == level)
		return 0;

	/*
	 * The vcpu irq_lines field was updated, wake up sleeping VCPUs and
	 * trigger a world-switch round on the running physical CPU to set the
	 * virtual IRQ/FIQ fields in the HCR appropriately.
	 */
	kvm_vcpu_kick(vcpu);

	return 0;
}

int kvm_vm_ioctl_irq_line(struct kvm *kvm, struct kvm_irq_level *irq_level)
{
	u32 irq = irq_level->irq;
	unsigned int irq_type, vcpu_idx, irq_num;
	int nrcpus = atomic_read(&kvm->online_vcpus);
	struct kvm_vcpu *vcpu = NULL;
	bool level = irq_level->level;

	irq_type = (irq >> KVM_ARM_IRQ_TYPE_SHIFT) & KVM_ARM_IRQ_TYPE_MASK;
	vcpu_idx = (irq >> KVM_ARM_IRQ_VCPU_SHIFT) & KVM_ARM_IRQ_VCPU_MASK;
	irq_num = (irq >> KVM_ARM_IRQ_NUM_SHIFT) & KVM_ARM_IRQ_NUM_MASK;

	trace_kvm_irq_line(irq_type, vcpu_idx, irq_num, irq_level->level);

	if (irq_type == KVM_ARM_IRQ_TYPE_CPU ||
	    irq_type == KVM_ARM_IRQ_TYPE_PPI) {
		if (vcpu_idx >= nrcpus)
			return -EINVAL;

		vcpu = kvm_get_vcpu(kvm, vcpu_idx);
		if (!vcpu)
			return -EINVAL;
	}

	switch (irq_type) {
	case KVM_ARM_IRQ_TYPE_CPU:
		if (irqchip_in_kernel(kvm))
			return -ENXIO;

		if (irq_num > KVM_ARM_IRQ_CPU_FIQ)
			return -EINVAL;

		return vcpu_interrupt_line(vcpu, irq_num, level);
#ifdef CONFIG_KVM_ARM_VGIC
	case KVM_ARM_IRQ_TYPE_PPI:
		if (!irqchip_in_kernel(kvm))
			return -ENXIO;

		if (irq_num < 16 || irq_num > 31)
			return -EINVAL;

		return kvm_vgic_inject_irq(kvm, vcpu->vcpu_id, irq_num, level);
	case KVM_ARM_IRQ_TYPE_SPI:
		if (!irqchip_in_kernel(kvm))
			return -ENXIO;

		if (irq_num < 32 || irq_num > KVM_ARM_IRQ_GIC_MAX)
			return -EINVAL;

		return kvm_vgic_inject_irq(kvm, 0, irq_num, level);
#endif
	}

	return -EINVAL;
}

long kvm_arch_vcpu_ioctl(struct file *filp,
			 unsigned int ioctl, unsigned long arg)
{
	struct kvm_vcpu *vcpu = filp->private_data;
	void __user *argp = (void __user *)arg;

	switch (ioctl) {
	case KVM_ARM_VCPU_INIT: {
		struct kvm_vcpu_init init;

		if (copy_from_user(&init, argp, sizeof init))
			return -EFAULT;

		return kvm_vcpu_set_target(vcpu, &init);

	}
	case KVM_SET_ONE_REG:
	case KVM_GET_ONE_REG: {
		struct kvm_one_reg reg;
		if (copy_from_user(&reg, argp, sizeof(reg)))
			return -EFAULT;
		if (ioctl == KVM_SET_ONE_REG)
			return kvm_arm_set_reg(vcpu, &reg);
		else
			return kvm_arm_get_reg(vcpu, &reg);
	}
	case KVM_GET_REG_LIST: {
		struct kvm_reg_list __user *user_list = argp;
		struct kvm_reg_list reg_list;
		unsigned n;

		if (copy_from_user(&reg_list, user_list, sizeof reg_list))
			return -EFAULT;
		n = reg_list.n;
		reg_list.n = kvm_arm_num_regs(vcpu);
		if (copy_to_user(user_list, &reg_list, sizeof reg_list))
			return -EFAULT;
		if (n < reg_list.n)
			return -E2BIG;
		return kvm_arm_copy_reg_indices(vcpu, user_list->reg);
	}
	default:
		return -EINVAL;
	}
}

int kvm_vm_ioctl_get_dirty_log(struct kvm *kvm, struct kvm_dirty_log *log)
{
	return -EINVAL;
}

long kvm_arch_vm_ioctl(struct file *filp,
		       unsigned int ioctl, unsigned long arg)
{

	switch (ioctl) {
#ifdef CONFIG_KVM_ARM_VGIC
	case KVM_CREATE_IRQCHIP: {
		struct kvm *kvm = filp->private_data;
		if (vgic_present)
			return kvm_vgic_init(kvm);
		else
			return -EINVAL;
	}
#endif
	default:
		return -EINVAL;
	}
}

static void cpu_set_vector(void *vector)
{
	unsigned long vector_ptr;

	vector_ptr = (unsigned long)vector;

	/*
	 * Set the HVBAR
	 */
	asm volatile (
		"mov	r0, %[vector_ptr]\n\t"
		"hvc	#0xff\n\t" : :
		[vector_ptr] "r" (vector_ptr) :
		"r0");
}

static void cpu_init_hyp_mode(void *vector)
{
	unsigned long pgd_ptr;
	unsigned long hyp_stack_ptr;
	unsigned long stack_page;
	unsigned long vector_ptr;

	/* Switch from the HYP stub to our own HYP init vector */
	__hyp_set_vectors((unsigned long)vector);

	pgd_ptr = virt_to_phys(hyp_pgd);
	stack_page = __get_cpu_var(kvm_arm_hyp_stack_page);
	hyp_stack_ptr = stack_page + PAGE_SIZE;
	vector_ptr = (unsigned long)__kvm_hyp_vector;

	/*
	 * Call initialization code, and switch to the full blown
	 * HYP code. The init code corrupts r12, so set the clobber
	 * list accordingly.
	 */
	asm volatile (
		"mov	r0, %[pgd_ptr]\n\t"
		"mov	r1, %[hyp_stack_ptr]\n\t"
		"mov	r2, %[vector_ptr]\n\t"
		"hvc	#0\n\t" : :
		[pgd_ptr] "r" (pgd_ptr),
		[hyp_stack_ptr] "r" (hyp_stack_ptr),
		[vector_ptr] "r" (vector_ptr) :
		"r0", "r1", "r2", "r12");
}

/**
 * Inits Hyp-mode on all online CPUs
 */
static int init_hyp_mode(void)
{
	phys_addr_t init_phys_addr;
	int cpu;
	int err = 0;

	/*
	 * It is probably enough to obtain the default on one
	 * CPU. It's unlikely to be different on the others.
	 */
	hyp_default_vectors = __hyp_get_vectors();

	/*
	 * Allocate stack pages for Hypervisor-mode
	 */
	for_each_possible_cpu(cpu) {
		unsigned long stack_page;

		stack_page = __get_free_page(GFP_KERNEL);
		if (!stack_page) {
			err = -ENOMEM;
			goto out_free_stack_pages;
		}

		per_cpu(kvm_arm_hyp_stack_page, cpu) = stack_page;
	}

	/*
	 * Execute the init code on each CPU.
	 *
	 * Note: The stack is not mapped yet, so don't do anything else than
	 * initializing the hypervisor mode on each CPU using a local stack
	 * space for temporary storage.
	 */
	init_phys_addr = virt_to_phys(__kvm_hyp_init);
	for_each_online_cpu(cpu) {
		smp_call_function_single(cpu, cpu_init_hyp_mode,
					 (void *)(long)init_phys_addr, 1);
	}

	/*
	 * Unmap the identity mapping
	 */
	hyp_idmap_teardown();

	/*
	 * Map the Hyp-code called directly from the host
	 */
	err = create_hyp_mappings(__kvm_hyp_code_start, __kvm_hyp_code_end);
	if (err) {
		kvm_err("Cannot map world-switch code\n");
		goto out_free_mappings;
	}

	/*
	 * Map the Hyp stack pages
	 */
	for_each_possible_cpu(cpu) {
		char *stack_page = (char *)per_cpu(kvm_arm_hyp_stack_page, cpu);
		err = create_hyp_mappings(stack_page, stack_page + PAGE_SIZE);

		if (err) {
			kvm_err("Cannot map hyp stack\n");
			goto out_free_mappings;
		}
	}

	/*
	 * Map the host VFP structures
	 */
	kvm_host_vfp_state = alloc_percpu(struct vfp_hard_struct);
	if (!kvm_host_vfp_state) {
		err = -ENOMEM;
		kvm_err("Cannot allocate host VFP state\n");
		goto out_free_mappings;
	}

	for_each_possible_cpu(cpu) {
		struct vfp_hard_struct *vfp;

		vfp = per_cpu_ptr(kvm_host_vfp_state, cpu);
		err = create_hyp_mappings(vfp, vfp + 1);

		if (err) {
			kvm_err("Cannot map host VFP state: %d\n", err);
			goto out_free_vfp;
		}
	}

	/*
	 * Init HYP view of VGIC
	 */
	err = kvm_vgic_hyp_init();
	if (!err)
		vgic_present = true;

	/*
	 * Init HYP architected timer support
	 */
	err = kvm_timer_hyp_init();
	if (err)
		goto out_free_mappings;

	return 0;
out_free_vfp:
	free_percpu(kvm_host_vfp_state);
out_free_mappings:
	free_hyp_pmds();
out_free_stack_pages:
	for_each_possible_cpu(cpu)
		free_page(per_cpu(kvm_arm_hyp_stack_page, cpu));
	return err;
}

/**
 * Initialize Hyp-mode and memory mappings on all CPUs.
 */
int kvm_arch_init(void *opaque)
{
	int err;

	if (!is_hyp_mode_available()) {
		kvm_err("HYP mode not available\n");
		return -ENODEV;
	}

	if (kvm_target_cpu() < 0) {
		kvm_err("Target CPU not supported!\n");
		return -ENODEV;
	}

	err = init_hyp_mode();
	if (err)
		goto out_err;

	kvm_coproc_table_init();
	return 0;
out_err:
	return err;
}

static void cpu_exit_hyp_mode(void *vector)
{
	cpu_set_vector(vector);

	/*
	 * Disable Hyp-MMU for each cpu, and switch back to the
	 * default vectors.
	 */
	asm volatile ("mov	r0, %[vector_ptr]\n\t"
		      "hvc	#0\n\t" : :
		      [vector_ptr] "r" (hyp_default_vectors) :
		      "r0");
}

static int exit_hyp_mode(void)
{
	phys_addr_t exit_phys_addr;
	int cpu;

	/*
	 * TODO: flush Hyp TLB in case idmap code overlaps.
	 * Note that we should do this in the monitor code when switching the
	 * HVBAR, but this is going  away and should be rather done in the Hyp
	 * mode change of HVBAR.
	 */
	hyp_idmap_setup();
	exit_phys_addr = virt_to_phys(__kvm_hyp_exit);
	BUG_ON(exit_phys_addr & 0x1f);

	/*
	 * Execute the exit code on each CPU.
	 *
	 * Note: The stack is not mapped yet, so don't do anything else than
	 * initializing the hypervisor mode on each CPU using a local stack
	 * space for temporary storage.
	 */
	for_each_online_cpu(cpu) {
		smp_call_function_single(cpu, cpu_exit_hyp_mode,
					 (void *)(long)exit_phys_addr, 1);
	}

	return 0;
}

void kvm_arch_exit(void)
{
	int cpu;

	exit_hyp_mode();

	free_hyp_pmds();
	free_percpu(kvm_host_vfp_state);
	for_each_possible_cpu(cpu) {
		free_page(per_cpu(kvm_arm_hyp_stack_page, cpu));
		per_cpu(kvm_arm_hyp_stack_page, cpu) = 0;
	}
}

static int arm_init(void)
{
	int rc = kvm_init(NULL, sizeof(struct kvm_vcpu), 0, THIS_MODULE);
	return rc;
}

static void __exit arm_exit(void)
{
	kvm_exit();
}

module_init(arm_init);
module_exit(arm_exit)
