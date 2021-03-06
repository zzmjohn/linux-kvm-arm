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

#ifndef __ARM_KVM_MMU_H__
#define __ARM_KVM_MMU_H__

/*
 * The architecture supports 40-bit IPA as input to the 2nd stage translations
 * and PTRS_PER_PGD2 could therefore be 1024.
 *
 * To save a bit of memory and to avoid alignment issues we assume 39-bit IPA
 * for now, but remember that the level-1 table must be aligned to its size.
 */
#define PTRS_PER_PGD2	512
#define PGD2_ORDER	get_order(PTRS_PER_PGD2 * sizeof(pgd_t))

int create_hyp_mappings(void *from, void *to);
int create_hyp_io_mappings(void *from, void *to, phys_addr_t);
void free_hyp_pmds(void);

int kvm_alloc_stage2_pgd(struct kvm *kvm);
void kvm_free_stage2_pgd(struct kvm *kvm);
int kvm_phys_addr_ioremap(struct kvm *kvm, phys_addr_t guest_ipa,
			  phys_addr_t pa, unsigned long size);

int kvm_handle_mmio_return(struct kvm_vcpu *vcpu, struct kvm_run *run);
int kvm_handle_guest_abort(struct kvm_vcpu *vcpu, struct kvm_run *run);

void kvm_mmu_free_memory_caches(struct kvm_vcpu *vcpu);

#endif /* __ARM_KVM_MMU_H__ */
