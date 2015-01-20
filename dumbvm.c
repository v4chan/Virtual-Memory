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
#include <array.h>
#include "opt-A3.h"
#include <syscall.h>

/*
 * Dumb MIPS-only "VM system" that is intended to only be just barely
 * enough to struggle off the ground. You should replace all of this
 * code while doing the VM assignment. In fact, starting in that
 * assignment, this file is not included in your kernel!
 */

/* under dumbvm, always have 48k of user stack */
#define DUMBVM_STACKPAGES    12

/*
 * Wrap rma_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;
struct lock *ft_lock;
struct lock *cm_lock;
struct array * frame_table;
//struct array *core_map;
paddr_t firstpaddr;
paddr_t lastpaddr;
bool vm_boot = false;

#if OPT_A3 

void
vm_bootstrap(void)
{
	firstpaddr = 405504;
	lastpaddr = 4194304;
	ft_lock = lock_create("ft_lock");
	//cm_lock = lock_create("cm_lock");
	frame_table = array_create();
	//core_map = array_create();
	lock_acquire(ft_lock);
	while (firstpaddr + PAGE_SIZE <= lastpaddr) {
		struct frame * f = kmalloc(sizeof(struct frame));
		f->paddr = firstpaddr;
		f->free = true;
		f->group_size = 0;
		//kprintf("f->free= %d,f->paddr= %d,",f->free,f->paddr);
		array_add(frame_table, f, NULL);
		firstpaddr = firstpaddr + PAGE_SIZE;
	}
	lock_release(ft_lock);
	//kprintf("%d %d\n",firstpaddr,lastpaddr);
	ram_getsize(&firstpaddr, &lastpaddr);
	//kprintf("%d %d\n",firstpaddr,lastpaddr);
	vm_boot = true;
}
#endif

static
paddr_t
getppages(unsigned long npages)
{
if (vm_boot == false) {
	paddr_t addr;
	spinlock_acquire(&stealmem_lock);
	addr = ram_stealmem(npages);
	spinlock_release(&stealmem_lock);
	return addr;
}
if (vm_boot == true && npages == 1) {
	paddr_t pa;
	int size_ft = array_num(frame_table);
	lock_acquire(ft_lock);
	for (int i = 0; i < size_ft; i++) {
		struct frame * f = array_get(frame_table, i);
		//kprintf("f->free= %d,f->paddr= %d,i = %d\n",f->free,f->paddr,i);
		if (f->free == true) {
			pa = f->paddr;
			f->free = false;
			f->group_size = npages;
			array_set(frame_table, i, f);
			lock_release(ft_lock);
			return pa;
		}
	}
	lock_release(ft_lock);
	panic("No more free frames available\n");
}
	int avail_frames = 0;
	paddr_t pa;
	int size_ft = array_num(frame_table);
	lock_acquire(ft_lock);
	for (int i = 0; i < size_ft; i++) {
		struct frame * f = array_get(frame_table, i);
		if (avail_frames == (int)npages) {
			pa = f->paddr - PAGE_SIZE*(i-1);
			avail_frames = 0;
			break;
		}
		else if (f->free == true) {
			avail_frames++;
		}
		else {
			avail_frames = 0;
		}
	}	

	for (int index = 0; index < size_ft; index++) {
		struct frame * f = array_get(frame_table, index);
		if (f->paddr == pa) {
			f->free = false;
			f->group_size = npages;
			array_set(frame_table, index, f);
			index++;
			while (index < (int)npages+index) {
				struct frame * ff = array_get(frame_table, index);
				ff->free = false;
				f->group_size = 0;
				array_set(frame_table, index, ff);
				index++;
			}
			break;
		}
	}
	lock_release(ft_lock);
	return pa;
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
if (vm_boot == false) {
	vaddr_t aaddr = addr;
	aaddr = aaddr + 0;
}
else {
	int size_ft = array_num(frame_table);
	//lock_acquire(ft_lock);
	for (int index = 0; index < size_ft; index++) {
		struct frame * f = array_get(frame_table, index);
		if (f->paddr == addr - MIPS_KSEG0 && f->group_size == 1) {
			f->free = true;
			f->group_size = 0;
			array_set(frame_table, index, f);
			break;
		}
		if (f->paddr == addr - MIPS_KSEG0) {
			int group_size = f->group_size;
			//kprintf("group size: %d\n",group_size);
			while (index < group_size+index) {
				struct frame * ff = array_get(frame_table, index);
				ff->free = true;
				ff->group_size = 0;
				array_set(frame_table, index, ff);
				index++;
			}
			break;
		}
	}
	//lock_release(ft_lock);
}
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

#if OPT_A3
int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	vaddr_t vbase1, vtop1, vbase2, vtop2, stackbase, stacktop;
	paddr_t paddr;
	int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;
	//struct proc * p = curproc;

	faultaddress &= PAGE_FRAME; // getting page number from address

	DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);

	
	switch (faulttype) {
	    case VM_FAULT_READONLY:
		/* We always create pages read-write, so we can't get this */
		sys__exit(faulttype);
		//panic("dumbvm: got VM_FAULT_READONLY\n");
		//return EFAULT;
		//break;
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
	
	vbase1 = as->as_vbase1;
	vtop1 = vbase1 + as->as_npages1 * PAGE_SIZE;
	vbase2 = as->as_vbase2;
	vtop2 = vbase2 + as->as_npages2 * PAGE_SIZE;
	stackbase = USERSTACK - DUMBVM_STACKPAGES * PAGE_SIZE;
	stacktop = USERSTACK;

	int page_index;

    
	if (faultaddress >= vbase1 && faultaddress < vtop1) {
		page_index = (faultaddress - vbase1)/PAGE_SIZE;
		struct page * p = array_get(as->page_table_code, page_index);
		paddr = p->paddr;
	}
	else if (faultaddress >= vbase2 && faultaddress < vtop2) {
		page_index = (faultaddress - vbase2)/PAGE_SIZE;
		struct page * p = array_get(as->page_table_data, page_index);
		paddr = p->paddr;
		//if ((paddr & PAGE_FRAME) != paddr) {
		//kprintf("paddr: %d, page_index: %d, page_table: %p, faultaddress: %d\n",paddr,page_index,as->page_table_data,faultaddress);
		//}
	}
	else if (faultaddress >= stackbase && faultaddress < stacktop) {
		page_index = (faultaddress - stackbase)/PAGE_SIZE;
		struct page * p = array_get(as->page_table_stack, page_index);
		paddr = p->paddr;
	}
	else {
		return EFAULT;
	}
	
	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);
	KASSERT(paddr >= firstpaddr || paddr <= lastpaddr);

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}
		if (faultaddress >= vbase1 && faultaddress < vtop1 && as->done_loading == true) { // text segment case
			ehi = faultaddress;
			elo = paddr | TLBLO_VALID;
			tlb_write(ehi,elo,i);
			splx(spl);
			return 0;
		}
		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
		DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}
	if (faultaddress >= vbase1 && faultaddress < vtop1 && as->done_loading == true) { // text segment case
			ehi = faultaddress;
			elo = paddr | TLBLO_VALID;
			tlb_random(ehi,elo);
			splx(spl);
			return 0;
	}
	ehi = faultaddress;
	elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
	//kprintf("!!! faultaddress: %d, paddr: %d\n",faultaddress,paddr);
	tlb_random(ehi,elo);
	splx(spl);
	return 0;
	kprintf("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
	splx(spl);
	return EFAULT;
}
#endif

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
	as->page_table_code = array_create();
	as->page_table_data = array_create();
	as->page_table_stack = array_create();

	return as;
}

void
as_destroy(struct addrspace *as)
{
	int npages1 = array_num(as->page_table_code);
	for (int i = npages1 - 1; i >=0; i--) {
		struct page * p = array_get(as->page_table_code,i);
		free_kpages(PADDR_TO_KVADDR(p->paddr));
		//array_remove(as->page_table_code,i);
	}
	//array_cleanup(as->page_table_code);
	int npages2 = array_num(as->page_table_data);
	for (int i = npages2 - 1; i >=0; i--) {
		struct page * p = array_get(as->page_table_data,i);
		free_kpages(PADDR_TO_KVADDR(p->paddr));
		//array_remove(as->page_table_data,i);
	}
	//array_cleanup(as->page_table_data);
	int npages3 = array_num(as->page_table_stack);
	for (int i = npages3 - 1; i >=0; i--) {
		struct page * p = array_get(as->page_table_stack,i);
		free_kpages(PADDR_TO_KVADDR(p->paddr));
		//array_remove(as->page_table_stack,i);
	}
	//array_cleanup(as->page_table_stack);
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

	//vaddr_t vaddr_temp = vaddr;
	if (as->as_vbase1 == 0) {
		as->as_vbase1 = vaddr;
		as->as_npages1 = npages;
		/*for (unsigned int index = 0; index < npages; index++) {
			struct core_map * cm = kmalloc(sizeof(struct core_map));
			cm->vaddr = vaddr_temp;
			cm->paddr = -1;
			cm->free = true;
			array_add(as->core_map, cm, NULL);
			vaddr_temp = vaddr_temp + PAGE_SIZE;
		}*/
		return 0;
	}

	if (as->as_vbase2 == 0) {
		as->as_vbase2 = vaddr;
		as->as_npages2 = npages;
		/*for (unsigned int index = 0; index < npages; index++) {
			struct core_map * cm = kmalloc(sizeof(struct core_map));
			cm->vaddr = vaddr_temp;
			cm->paddr = -1;
			cm->free = true;
			array_add(as->core_map, cm, NULL);
			vaddr_temp = vaddr_temp + PAGE_SIZE;
		}*/
		return 0;
	}

	/*
	 * Support for more than two regions is not available.
	 */
	kprintf("dumbvm: Warning: too many regions\n");
	return EUNIMP;
}
/*
static
void
as_zero_region(paddr_t paddr, unsigned npages)
{
	bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
}
*/
int
as_prepare_load(struct addrspace *as)
{
	KASSERT(as->as_pbase1 == 0);
	KASSERT(as->as_pbase2 == 0);
	KASSERT(as->as_stackpbase == 0);

	struct page *curpage;
	for (unsigned int index = 0; index < as->as_npages1; index++) {
		curpage = kmalloc(sizeof(struct page));
		curpage->vaddr = as->as_vbase1 + index * PAGE_SIZE;
		curpage->paddr = getppages(1);
		array_add(as->page_table_code, curpage, NULL);
	}
	for (unsigned int index = 0; index < as->as_npages2; index++) {
		curpage = kmalloc(sizeof(struct page));
		curpage->vaddr = as->as_vbase2 + index * PAGE_SIZE;
		curpage->paddr = getppages(1);
		array_add(as->page_table_data, curpage, NULL);
	}
	for (unsigned int index = 0; index < DUMBVM_STACKPAGES; index++) {
		curpage = kmalloc(sizeof(struct page));
		curpage->vaddr = USERSTACK - DUMBVM_STACKPAGES * PAGE_SIZE + index * PAGE_SIZE;
		curpage->paddr = getppages(1);
		array_add(as->page_table_stack, curpage, NULL);
	}

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
	//KASSERT(as->as_stackpbase != 0);
	(void)as;
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
	old = old + 0;
	new->as_vbase1 = old->as_vbase1;
	new->as_npages1 = old->as_npages1;
	new->as_vbase2 = old->as_vbase2;
	new->as_npages2 = old->as_npages2;
	new->page_table_code = old->page_table_code;
	new->page_table_data = old->page_table_data;
	new->page_table_stack = old->page_table_stack;
	new->done_loading = old->done_loading;

	/* (Mis)use as_prepare_load to allocate some physical memory. */
	/*if (as_prepare_load(new)) {
		as_destroy(new);
		return ENOMEM;
	}*/

	//KASSERT(new->as_pbase1 != 0);
	//KASSERT(new->as_pbase2 != 0);
	//KASSERT(new->as_stackpbase != 0);

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
