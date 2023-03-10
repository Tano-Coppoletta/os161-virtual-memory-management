/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <cpu.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <opt-paging.h>
#include <pt.h>
#include <types.h>
#include <mainbus.h>
#include "vm_tlb.h"
#include <syscall.h>
#include <vmstats.h>

/* under dumbvm, always have 72k of user stack */
/* (this must be > 64K so argument blocks of size ARG_MAX will fit) */
#define DUMBVM_STACKPAGES    18

/*
 * Wrap ram_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;

void
vm_bootstrap(void)
{
	vm_enabled = 0;
	uint32_t i;
	spinlock_init(&vm_lock);
	spinlock_init(&k_lock);

	k_frames = kmalloc(MAX_PROCESSES * sizeof(*k_frames));
	for(i = 0; i < MAX_PROCESSES; i++){
		if(i == 0){
			k_frames[i].prev = -1;
			k_frames[i].next = 1;
		}else if(i == MAX_PROCESSES - 1){
			k_frames[i].next = -1;
			k_frames[i].prev = i - 1;
		}else{
			k_frames[i].prev = i - 1;
			k_frames[i].next = i + 1;
		}
		k_frames[i].owner = 0;
		k_frames[i].n_pages = 0;
		k_frames[i].start_frame_n_to_remove = 0;
		k_frames[i].vaddr_to_free = 0;
	}
	start_index_k = -1;
	start_free_index = 0;
	// Swap area init
	char swap_file_name[] = "lhd0raw:";
	ST = swapTableInit(swap_file_name);

	// IPT init
	size_t ram_size = mainbus_ramsize();
	spinlock_acquire(&stealmem_lock);
	paddr_t ram_user_base = ram_stealmem(0);
	spinlock_release(&stealmem_lock);
	// (- PAGE_SIZE) because of kmalloc which is going to call alloc_kpages which calls ram_stealmem starting from the first address we should keep track of
	// in this way we don't keep track of the page where the IPT and ST are stored
	IPT = pageTInit((ram_size-ram_user_base-PAGES_FOR_IPT*PAGE_SIZE)/PAGE_SIZE);
	vm_enabled = 1;
	/* Now we can start keeping track of VM stats */
	stat_bootstrap();
}

static
paddr_t
getppages(unsigned long npages)
{
	paddr_t addr;

	spinlock_acquire(&stealmem_lock);

	addr = ram_stealmem(npages);

	spinlock_release(&stealmem_lock);
	return addr;
}

/* Allocate/free some kernel-space virtual pages */
vaddr_t
alloc_kpages(unsigned npages)
{
	int spl = splhigh();
	paddr_t pa;
	if(vm_enabled) {
		//alloc n contiguous pages
		pa = alloc_n_contiguos_pages(npages, IPT);
	}else{
		pa = getppages(npages);
		if (pa==0) {
			return 0;
		}

	}
	splx(spl);
	return PADDR_TO_KVADDR(pa);
}

void
free_kpages(vaddr_t addr)
{
	int frame_n, i;
	uint32_t j;
	int spl = splhigh();
	if(vm_enabled) {
		spinlock_acquire(&k_lock);

		addr &= PAGE_FRAME;
		for(i = start_index_k; i != -1 && k_frames[i].vaddr_to_free != addr; i = k_frames[i].next);
		if(i == -1)
			panic("Where is that frame?!\n");
		for(frame_n = k_frames[i].start_frame_n_to_remove, j = 0; j < k_frames[i].n_pages; frame_n++, j++){
			remove_page(IPT, frame_n);
		}
		frame_n_k += k_frames[i].n_pages;
		if(i == start_index_k){
			start_index_k = k_frames[i].next;
			if(start_index_k != -1)
				k_frames[start_index_k].prev = -1;
		}else{
			k_frames[k_frames[i].prev].next = k_frames[i].next;
			if(k_frames[i].next != -1)
				k_frames[k_frames[i].next].prev = k_frames[i].prev;
		}
		k_frames[start_free_index].prev = i;
		k_frames[i].next = start_free_index;
		k_frames[i].prev = -1;
		start_free_index = i;
		spinlock_release(&k_lock);
	}else{
		/* nothing - leak the memory. */
	}
	splx(spl);
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("dumbvm tried to do tlb shootdown?!\n");
}

void
vm_shutdown(void) 
{
	print_stats();
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	//vaddr_t vbase1, vtop1, vbase2, vtop2, stackbase, stacktop;
	int paddr;
	//uint32_t ehi, elo;
	struct addrspace *as;
	int spl;

	faultaddress &= PAGE_FRAME;

	DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);

	switch (faulttype) {
	    case VM_FAULT_READONLY:
			//try to access a read-only segment causes a fault, terminate the process
			kprintf("\nWrite attempt on read-only code segment!\nI think I'll end the process...\n");
			sys__exit(0);
			break;
	    case VM_FAULT_READ:
			
			break;
	    case VM_FAULT_WRITE:
		
		break;
	    default:
		return EINVAL;
	}

	if (curproc == NULL) {
		/*
		 * No process. This is probably a kernel fault early
		 * in boot. Return EFAULT so as to panic instead of
		 * getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

	as = proc_getas();
	if (as == NULL) {
		/*
		 * No address space set up. This is probably also a
		 * kernel fault early in boot.
		 */
		return EFAULT;
	}

	/* Assert that the address space has been set up properly. */
	KASSERT(as->as_vbase1 != 0);
	//KASSERT(as->as_pbase1 != 0);
	KASSERT(as->as_npages1 != 0);
	KASSERT(as->as_vbase2 != 0);
	//KASSERT(as->as_pbase2 != 0);
	KASSERT(as->as_npages2 != 0);
	//KASSERT(as->as_stackpbase != 0);
	KASSERT((as->as_vbase1 & PAGE_FRAME) == as->as_vbase1);
	//KASSERT((as->as_pbase1 & PAGE_FRAME) == as->as_pbase1);
	KASSERT((as->as_vbase2 & PAGE_FRAME) == as->as_vbase2);
	//KASSERT((as->as_pbase2 & PAGE_FRAME) == as->as_pbase2);
	//KASSERT((as->as_stackpbase & PAGE_FRAME) == as->as_stackpbase);
	/*
	vbase1 = as->as_vbase1;
	vtop1 = vbase1 + as->as_npages1 * PAGE_SIZE;
	vbase2 = as->as_vbase2;
	vtop2 = vbase2 + as->as_npages2 * PAGE_SIZE;
	stackbase = USERSTACK - DUMBVM_STACKPAGES * PAGE_SIZE;
	stacktop = USERSTACK;

	if (faultaddress >= vbase1 && faultaddress < vtop1) {
		paddr = (faultaddress - vbase1) + as->as_pbase1;
	}
	else if (faultaddress >= vbase2 && faultaddress < vtop2) {
		paddr = (faultaddress - vbase2) + as->as_pbase2;
	}
	else if (faultaddress >= stackbase && faultaddress < stacktop) {
		paddr = (faultaddress - stackbase) + as->as_stackpbase;
	}
	else {
		return EFAULT;
	}*/

	/* make sure it's page-aligned */
	//KASSERT((paddr & PAGE_FRAME) == paddr);

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();
	//spinlock_acquire(&vm_lock);
	if(faultaddress <= MIPS_KSEG0) {
		/* statistics */ add_TLB_fault();
		//retrieve the frame number in the page table
		paddr = getFrameAddress(IPT,(faultaddress & PAGE_FRAME) >> 12, false);
		if(paddr==-1){
			//PAGE FAULT
			/* Page was not already in memory, we need to handle page fault */
			paddr = pageIn(IPT, curproc->p_pid, faultaddress, ST);
		} else {
			/* Page is already in memory, we just need to reload the entry in the TLB */
			/* statistics */ add_TLB_reload();
		}
		paddr = paddr & PAGE_FRAME;
		TLB_Insert(faultaddress,paddr);
		//add to tlb
		splx(spl);
		//spinlock_release(&vm_lock);
		return 0;
	}
	splx(spl);
	spinlock_release(&vm_lock);
	//spinlock_release(&vm_lock);
	return EFAULT;
}


