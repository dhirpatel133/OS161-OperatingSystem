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
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include "opt-A3.h"

/*
 * Dumb MIPS-only "VM system" that is intended to only be just barely
 * enough to struggle off the ground.
 */

/* under dumbvm, always have 48k of user stack */
#define DUMBVM_STACKPAGES    12

/*
 * Wrap rma_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;

#if OPT_A3
//version 2 of coremap (only check if page is free or not)
struct coreMap {
	int inUse;
};
static struct spinlock coremap_lock = SPINLOCK_INITIALIZER;
struct coreMap *cMap = NULL;
bool isBootstrapped = false;
paddr_t low = 0;
paddr_t high = 0;
int pageEntries = 0;
#endif

void
vm_bootstrap(void)
{
	#if OPT_A3
	ram_getsize(&low, &high); //get the starting and ending physical address of memory
	pageEntries = (high - low) / PAGE_SIZE; //total number of pages including coremap struct
	//kprintf("Total pages with coremap: %d\n", pageEntries);

	int cMapSize = pageEntries * sizeof(struct coreMap); //coremap size
	cMapSize = ROUNDUP(cMapSize, PAGE_SIZE); //make coremap size a multiple of 4096 by rounding it up
	//kprintf("cMap size: %d\n", cMapSize);

	cMap = (struct coreMap *) PADDR_TO_KVADDR(low); //get kernel virtual address of starting point including coremap
	low += cMapSize; //update starting so that it does not include the coremap (i.e. track memory after the coremap)

	pageEntries = (high - low) / PAGE_SIZE; //get the total number of pages, this time it excludes the coremap
	//kprintf("New total pages size without coremap: %d\n", pageEntries);

	//initialize all pages after the coremap to 0 (i.e to denote free pages)
	for (int i = 0; i < pageEntries; i++) {
		cMap[i].inUse = 0;
	}
	isBootstrapped = true; //set bootstrap flag to true (will be used later in )
	#endif
	/* Do nothing. */
}

static paddr_t getppages(unsigned long npages) {
	paddr_t addr;

	spinlock_acquire(&stealmem_lock);
	#if OPT_A3
		if (isBootstrapped) {
			//kprintf("pages to print %lu\n", npages);
			spinlock_acquire(&coremap_lock);
			unsigned long count = npages;
			bool actualStart = true;
			int startFrame = 0;
			//according to A3 guide, we are implementing contiguous memory so we don't need to 
			//check explicitly for paging. Only need to find a contiguous block of size npages.
			for (int i = 0; i < pageEntries; i++) {
				if (count == 0) {
					break;
				}
				if (cMap[i].inUse == 0) {
					count--;
					if (actualStart) {
						startFrame = i;
						actualStart = false;
					}
				} else {
					count = npages;
					actualStart = true;
				}
			}
			if (count == 0) {
				//addres of a starting block is: startIndex * pageSize + offset 
				//(offset is the starting address of memory without the coremap so in this case it's low)
				addr = (paddr_t) (startFrame * PAGE_SIZE + low);
				// int addr2 = (int) addr;
				// kprintf("starting address of block: %d\n", addr2);
				// kprintf("index of starting block: %d\n", startFrame);
				
				//update the coremap so that it occupies a block of npages contiguously
				int indx = startFrame;
				for (unsigned long i = 1; i <= npages; i++) {
					cMap[indx].inUse = (int) i;
					++indx;
				}
				// //print coreMap
				// for (int i = 0; i < pageEntries; i++) {
				// 	kprintf("[%d]: %d\n", i, cMap[i].inUse);
				// }
			} else {
				addr = 0;
				kprintf("Not enough memory available for contiguous pages\n");
			}
			spinlock_release(&coremap_lock);
		} else {
			addr = ram_stealmem(npages);
		}
	#else
		addr = ram_stealmem(npages);
	#endif
		spinlock_release(&stealmem_lock);
		return addr;
}

/* Allocate/free some kernel-space virtual pages */
vaddr_t 
alloc_kpages(int npages)
{
	paddr_t pa;
	pa = getppages(npages);
	if (pa==0) {
		return 0;
	}
	return PADDR_TO_KVADDR(pa);
}

void 
free_kpages(vaddr_t addr)
{
	#if OPT_A3
		//kprintf("arguement addr: %d\n", (int) addr);
		paddr_t physicalAddr = KVADDR_TO_PADDR(addr); //obtain physical address from the kernel virtual address
		//kprintf("Free block with starting address: %d\n", (int) physicalAddr);
		spinlock_acquire(&coremap_lock);
		int pageIndex = (physicalAddr - low) / PAGE_SIZE; //get the starting index of the allocated block we want to free
		//kprintf("Free block with starting index: %d\n", pageIndex);	
		//int isPageFree = 0;
		int currentPage = 0;
		int successor = 0;
		//loop until you encounter the end of the contiguous block
		while (true) {
			currentPage = cMap[pageIndex].inUse;
			cMap[pageIndex].inUse = 0;
			pageIndex++;
			successor = cMap[pageIndex].inUse;
			if (successor - currentPage != 1) {
				break;
			}
		}
		spinlock_release(&coremap_lock);
	#else
		/* nothing - leak the memory. */
		(void)addr;
	#endif
}

void
vm_tlbshootdown_all(void)
{
	panic("dumbvm tried to do tlb shootdown?!\n");
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("dumbvm tried to do tlb shootdown?!\n");
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	vaddr_t vbase1, vtop1, vbase2, vtop2, stackbase, stacktop;
	paddr_t paddr;
	int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;

	faultaddress &= PAGE_FRAME;

	DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);

	switch (faulttype) {
	    case VM_FAULT_READONLY:
		#if OPT_A3
			return EFAULT; /* Bad memory reference because memory is read only and you shouldn't write to it*/
		#else
			/* We always create pages read-write, so we can't get this */
			panic("dumbvm: got VM_FAULT_READONLY\n");
		#endif
	    case VM_FAULT_READ:
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

	as = curproc_getas();
	if (as == NULL) {
		/*
		 * No address space set up. This is probably also a
		 * kernel fault early in boot.
		 */
		return EFAULT;
	}

	/* Assert that the address space has been set up properly. */
	KASSERT(as->as_vbase1 != 0);
	KASSERT(as->as_pbase1 != 0);
	KASSERT(as->as_npages1 != 0);
	KASSERT(as->as_vbase2 != 0);
	KASSERT(as->as_pbase2 != 0);
	KASSERT(as->as_npages2 != 0);
	KASSERT(as->as_stackpbase != 0);
	KASSERT((as->as_vbase1 & PAGE_FRAME) == as->as_vbase1);
	KASSERT((as->as_pbase1 & PAGE_FRAME) == as->as_pbase1);
	KASSERT((as->as_vbase2 & PAGE_FRAME) == as->as_vbase2);
	KASSERT((as->as_pbase2 & PAGE_FRAME) == as->as_pbase2);
	KASSERT((as->as_stackpbase & PAGE_FRAME) == as->as_stackpbase);

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
	}

	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}
		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
		#if OPT_A3
			if (as->as_isLoadElfComplete && faultaddress >= vbase1 && faultaddress < vtop1) {
				elo &= ~TLBLO_DIRTY;
			}
		#endif
		DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}

	//if TLB is full then write to a random position instead of printing an error.
	#if  OPT_A3
		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
		if (as->as_isLoadElfComplete && faultaddress >= vbase1 && faultaddress < vtop1) {
			elo &= ~TLBLO_DIRTY;
		}
		tlb_random(ehi, elo);
		splx(spl);
		return 0;
	#else
		kprintf("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
		splx(spl);
		return EFAULT;
	#endif
}

struct addrspace *
as_create(void)
{
	struct addrspace *as = kmalloc(sizeof(struct addrspace));
	if (as==NULL) {
		return NULL;
	}

	as->as_vbase1 = 0;
	as->as_pbase1 = 0;
	as->as_npages1 = 0;
	as->as_vbase2 = 0;
	as->as_pbase2 = 0;
	as->as_npages2 = 0;
	as->as_stackpbase = 0;
	#if OPT_A3
		as->as_isLoadElfComplete = false;
	#endif

	return as;
}

void
as_destroy(struct addrspace *as)
{
	#if OPT_A3
		free_kpages(PADDR_TO_KVADDR(as->as_pbase1));
		free_kpages(PADDR_TO_KVADDR(as->as_pbase2));
		free_kpages(PADDR_TO_KVADDR(as->as_stackpbase));
	#endif
		kfree(as);
}

void
as_activate(void)
{
	int i, spl;
	struct addrspace *as;

	as = curproc_getas();
#ifdef UW
        /* Kernel threads don't have an address spaces to activate */
#endif
	if (as == NULL) {
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

void
as_deactivate(void)
{
	/* nothing */
}

int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		 int readable, int writeable, int executable)
{
	size_t npages; 

	/* Align the region. First, the base... */
	sz += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	/* ...and now the length. */
	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;

	npages = sz / PAGE_SIZE;

	/* We don't use these - all pages are read-write */
	(void)readable;
	(void)writeable;
	(void)executable;

	if (as->as_vbase1 == 0) {
		as->as_vbase1 = vaddr;
		as->as_npages1 = npages;
		return 0;
	}

	if (as->as_vbase2 == 0) {
		as->as_vbase2 = vaddr;
		as->as_npages2 = npages;
		return 0;
	}

	/*
	 * Support for more than two regions is not available.
	 */
	kprintf("dumbvm: Warning: too many regions\n");
	return EUNIMP;
}

static
void
as_zero_region(paddr_t paddr, unsigned npages)
{
	bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
}

int
as_prepare_load(struct addrspace *as)
{
	KASSERT(as->as_pbase1 == 0);
	KASSERT(as->as_pbase2 == 0);
	KASSERT(as->as_stackpbase == 0);

	as->as_pbase1 = getppages(as->as_npages1);
	// kprintf("\nnpages1: %d\n", (int)as->as_npages1);
	// kprintf("pbase1: %d\n", (int)as->as_pbase1);
	if (as->as_pbase1 == 0) {
		return ENOMEM;
	}

	as->as_pbase2 = getppages(as->as_npages2);
	// kprintf("\nnpages2: %d\n", (int)as->as_npages2);
	// kprintf("pbase2: %d\n", (int)as->as_pbase2);
	if (as->as_pbase2 == 0) {
		return ENOMEM;
	}

	as->as_stackpbase = getppages(DUMBVM_STACKPAGES);
	// kprintf("stackpbase: %d\n", (int)as->as_stackpbase);
	if (as->as_stackpbase == 0) {
		return ENOMEM;
	}
	
	as_zero_region(as->as_pbase1, as->as_npages1);
	as_zero_region(as->as_pbase2, as->as_npages2);
	as_zero_region(as->as_stackpbase, DUMBVM_STACKPAGES);

	return 0;
}

int
as_complete_load(struct addrspace *as)
{
	(void)as;
	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	KASSERT(as->as_stackpbase != 0);

	*stackptr = USERSTACK;
	return 0;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *new;

	new = as_create();
	if (new==NULL) {
		return ENOMEM;
	}

	new->as_vbase1 = old->as_vbase1;
	new->as_npages1 = old->as_npages1;
	new->as_vbase2 = old->as_vbase2;
	new->as_npages2 = old->as_npages2;

	/* (Mis)use as_prepare_load to allocate some physical memory. */
	if (as_prepare_load(new)) {
		as_destroy(new);
		return ENOMEM;
	}

	KASSERT(new->as_pbase1 != 0);
	KASSERT(new->as_pbase2 != 0);
	KASSERT(new->as_stackpbase != 0);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase1),
		(const void *)PADDR_TO_KVADDR(old->as_pbase1),
		old->as_npages1*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase2),
		(const void *)PADDR_TO_KVADDR(old->as_pbase2),
		old->as_npages2*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_stackpbase),
		(const void *)PADDR_TO_KVADDR(old->as_stackpbase),
		DUMBVM_STACKPAGES*PAGE_SIZE);
	
	*ret = new;
	return 0;
}
