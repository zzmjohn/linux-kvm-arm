#
# Makefile for Kernel-based Virtual Machine module
#

plus_virt := $(call as-instr,.arch_extension virt,+virt)
ifeq ($(plus_virt),+virt)
	plus_virt_def := -DREQUIRES_VIRT=1
endif

ccflags-y += -Ivirt/kvm -Iarch/arm/kvm
CFLAGS_arm.o := -I. $(plus_virt_def)
CFLAGS_mmu.o := -I.

AFLAGS_init.o := -Wa,-march=armv7-a$(plus_virt)
AFLAGS_interrupts.o := -Wa,-march=armv7-a$(plus_virt)

obj-$(CONFIG_KVM_ARM_HOST) += init.o interrupts.o exports.o

obj-$(CONFIG_KVM_ARM_HOST) += $(addprefix ../../../virt/kvm/, kvm_main.o coalesced_mmio.o)
obj-$(CONFIG_KVM_ARM_HOST) += arm.o guest.o mmu.o emulate.o reset.o coproc.o
obj-$(CONFIG_KVM_ARM_VGIC) += vgic.o
obj-$(CONFIG_KVM_ARM_TIMER) += timer.o
