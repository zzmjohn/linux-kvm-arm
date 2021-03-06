#if !defined(_TRACE_KVM_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_KVM_H

#include <linux/tracepoint.h>

#undef TRACE_SYSTEM
#define TRACE_SYSTEM kvm

/*
 * Tracepoints for entry/exit to guest
 */
TRACE_EVENT(kvm_entry,
	TP_PROTO(unsigned long vcpu_pc),
	TP_ARGS(vcpu_pc),

	TP_STRUCT__entry(
		__field(	unsigned long,	vcpu_pc		)
	),

	TP_fast_assign(
		__entry->vcpu_pc		= vcpu_pc;
	),

	TP_printk("PC: 0x%08lx", __entry->vcpu_pc)
);

TRACE_EVENT(kvm_exit,
	TP_PROTO(unsigned long vcpu_pc),
	TP_ARGS(vcpu_pc),

	TP_STRUCT__entry(
		__field(	unsigned long,	vcpu_pc		)
	),

	TP_fast_assign(
		__entry->vcpu_pc		= vcpu_pc;
	),

	TP_printk("PC: 0x%08lx", __entry->vcpu_pc)
);

TRACE_EVENT(kvm_guest_fault,
	TP_PROTO(unsigned long vcpu_pc, unsigned long hsr,
		 unsigned long hdfar, unsigned long hifar,
		 unsigned long ipa),
	TP_ARGS(vcpu_pc, hsr, hdfar, hifar, ipa),

	TP_STRUCT__entry(
		__field(	unsigned long,	vcpu_pc		)
		__field(	unsigned long,	hsr		)
		__field(	unsigned long,	hdfar		)
		__field(	unsigned long,	hifar		)
		__field(	unsigned long,	ipa		)
	),

	TP_fast_assign(
		__entry->vcpu_pc		= vcpu_pc;
		__entry->hsr			= hsr;
		__entry->hdfar			= hdfar;
		__entry->hifar			= hifar;
		__entry->ipa			= ipa;
	),

	TP_printk("guest fault at PC %#08lx (hdfar %#08lx, hifar %#08lx, "
		  "ipa %#08lx, hsr %#08lx",
		  __entry->vcpu_pc, __entry->hdfar, __entry->hifar,
		  __entry->hsr, __entry->ipa)
);

TRACE_EVENT(kvm_irq_line,
	TP_PROTO(unsigned int type, int vcpu_idx, int irq_num, int level),
	TP_ARGS(type, vcpu_idx, irq_num, level),

	TP_STRUCT__entry(
		__field(	unsigned int,	type		)
		__field(	int,		vcpu_idx	)
		__field(	int,		irq_num		)
		__field(	int,		level		)
	),

	TP_fast_assign(
		__entry->type		= type;
		__entry->vcpu_idx	= vcpu_idx;
		__entry->irq_num	= irq_num;
		__entry->level		= level;
	),

	TP_printk("Inject %s interrupt (%d), vcpu->idx: %d, num: %d, level: %d",
		  (__entry->type == KVM_ARM_IRQ_TYPE_CPU) ? "CPU" :
		  (__entry->type == KVM_ARM_IRQ_TYPE_PPI) ? "VGIC PPI" :
		  (__entry->type == KVM_ARM_IRQ_TYPE_SPI) ? "VGIC SPI" : "UNKNOWN",
		  __entry->type, __entry->vcpu_idx, __entry->irq_num, __entry->level)
);

TRACE_EVENT(kvm_mmio_emulate,
	TP_PROTO(unsigned long vcpu_pc, unsigned long instr,
		 unsigned long cpsr),
	TP_ARGS(vcpu_pc, instr, cpsr),

	TP_STRUCT__entry(
		__field(	unsigned long,	vcpu_pc		)
		__field(	unsigned long,	instr		)
		__field(	unsigned long,	cpsr		)
	),

	TP_fast_assign(
		__entry->vcpu_pc		= vcpu_pc;
		__entry->instr			= instr;
		__entry->cpsr			= cpsr;
	),

	TP_printk("Emulate MMIO at: 0x%08lx (instr: %08lx, cpsr: %08lx)",
		  __entry->vcpu_pc, __entry->instr, __entry->cpsr)
);

/* Architecturally implementation defined CP15 register access */
TRACE_EVENT(kvm_emulate_cp15_imp,
	TP_PROTO(unsigned long Op1, unsigned long Rt1, unsigned long CRn,
		 unsigned long CRm, unsigned long Op2, bool is_write),
	TP_ARGS(Op1, Rt1, CRn, CRm, Op2, is_write),

	TP_STRUCT__entry(
		__field(	unsigned int,	Op1		)
		__field(	unsigned int,	Rt1		)
		__field(	unsigned int,	CRn		)
		__field(	unsigned int,	CRm		)
		__field(	unsigned int,	Op2		)
		__field(	bool,		is_write	)
	),

	TP_fast_assign(
		__entry->is_write		= is_write;
		__entry->Op1			= Op1;
		__entry->Rt1			= Rt1;
		__entry->CRn			= CRn;
		__entry->CRm			= CRm;
		__entry->Op2			= Op2;
	),

	TP_printk("Implementation defined CP15: %s\tp15, %u, r%u, c%u, c%u, %u",
			(__entry->is_write) ? "mcr" : "mrc",
			__entry->Op1, __entry->Rt1, __entry->CRn,
			__entry->CRm, __entry->Op2)
);

TRACE_EVENT(kvm_wfi,
	TP_PROTO(unsigned long vcpu_pc),
	TP_ARGS(vcpu_pc),

	TP_STRUCT__entry(
		__field(	unsigned long,	vcpu_pc		)
	),

	TP_fast_assign(
		__entry->vcpu_pc		= vcpu_pc;
	),

	TP_printk("guest executed wfi at: 0x%08lx", __entry->vcpu_pc)
);


#endif /* _TRACE_KVM_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH arch/arm/kvm
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE trace

/* This part must be outside protection */
#include <trace/define_trace.h>
