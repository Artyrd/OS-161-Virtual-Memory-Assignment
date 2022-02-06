/*SID doesnt matter
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *    The President and Fellows of Harvard College.
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
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <proc.h>

/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 *
 * UNSW: If you use ASST3 config as required, then this file forms
 * part of the VM subsystem.
 *
 */

struct addrspace *
as_create(void)
{
    struct addrspace *as;

    as = kmalloc(sizeof(struct addrspace));
    if (as == NULL) {
        return NULL;
    }
    as->region = NULL; // initialise region in as_define region

    /* initialise page table */

    // pagetable level 1: array of pointers
    as->pagetable = kmalloc(PT1_SIZE * sizeof(paddr_t**));
    if (as->pagetable == NULL) {
        kfree(as);
        return NULL;
    }
    for (int i = 0; i < PT1_SIZE; i++) {
        as->pagetable[i] = NULL;
    }
    /* lazy data structure, so lower levels won't get initialised until vm_fault */

    return as;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
    struct addrspace *newas;
    struct as_region *new_region;

    newas = as_create();
    if (newas==NULL) {
        return ENOMEM;
    }

    struct as_region *curr = newas->region;
    struct as_region *curr_old = old->region; // added this to not mess up old

    while (curr_old != NULL) {
        // make new_region to copy curr_old's region into
        new_region = kmalloc(sizeof(struct as_region));
        if (new_region == NULL) {
            as_destroy(newas);
            return ENOMEM;
        }
        new_region->vbase = curr_old->vbase;
        new_region->npages = curr_old->npages;
        new_region->readable = curr_old->readable;
        new_region->writeable = curr_old->writeable;
        new_region->executable = curr_old->executable;
        new_region->prev_read = curr_old->prev_read;
        new_region->prev_write = curr_old->prev_write;
        new_region->prev_exec = curr_old->prev_exec;
        new_region->next = NULL; // don't let it point to the same memory as curr_old->next

        if (newas->region == NULL) {
            newas->region = new_region;
        } else {
            curr->next = new_region;
        }
        curr_old = curr_old->next;
        curr = new_region;
    }

    /* copy page table */
    int retval = 0;
    newas->pagetable = pt_copy(old->pagetable, &retval);
    if (retval) {
        as_destroy(newas);
        return retval;
    }

    *ret = newas;

    return 0;
}

void
as_destroy(struct addrspace *as)
{
    pt_destroy(as->pagetable);

    struct as_region *curr = as->region;
    struct as_region *next;

    while (curr != NULL) {
        next = curr->next;
        kfree(curr);
        curr = next;
    }

    kfree(as);
}

void
as_activate(void)
{
    int i, spl;
    struct addrspace *as;

    as = proc_getas();
    if (as == NULL) {
        /*
         * Kernel thread without an address space; leave the
         * prior address space in place.
         */
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
    /*
     * Write this. For many designs it won't need to actually do
     * anything. See proc.c for an explanation of why it (might)
     * be needed.
     */

    int i, spl;
    struct addrspace *as;

    as = proc_getas();
    if (as == NULL) {
        /*
         * Kernel thread without an address space; leave the
         * prior address space in place.
         */
        return;
    }

    /* Disable interrupts on this CPU while frobbing the TLB. */
    spl = splhigh();

    for (i=0; i<NUM_TLB; i++) {
        tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
    }

    splx(spl);
}

/*
 * Set up a segment at virtual address VADDR of size MEMSIZE. The
 * segment in memory extends from VADDR up to (but not including)
 * VADDR+MEMSIZE.
 *
 * The READABLE, WRITEABLE, and EXECUTABLE flags are set if read,
 * write, or execute permission should be set on the segment. At the
 * moment, these are ignored. When you write the VM system, you may
 * want to implement them.
 */
int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t memsize,
         int readable, int writeable, int executable)
{
    size_t npages;

    memsize += vaddr & ~(vaddr_t)PAGE_FRAME;
    vaddr &= PAGE_FRAME;
    memsize = (memsize + PAGE_SIZE - 1) & PAGE_FRAME;

    npages = memsize / PAGE_SIZE;

    // initialise new region
    struct as_region *new = kmalloc(sizeof(struct as_region));
    if (new == NULL) {
        return ENOMEM;
    }
    new->vbase = vaddr;
    new->npages = npages;

    new->readable = readable;
    new->writeable = writeable;
    new->executable = executable;

    new->prev_read = readable;
    new->prev_write = writeable;
    new->prev_exec = executable;

    new->next = NULL;

    // change first region if not valid
    if (as->region == NULL) {
        as->region = new;

        return 0;
    }
    // else if an existing region exists, navigate to the end and append
    struct as_region *curr = as->region;
    vaddr_t bot;
    vaddr_t top;
    // check for overlapping regions while navigating to end of list
    while (curr->next != NULL) {
        bot = curr->vbase;
        top = bot + PAGE_SIZE * curr->npages;

        if ((vaddr > bot && vaddr < top) ||
            (vaddr + memsize > bot && vaddr + memsize < top)) {
            return EINVAL;
        }

        curr = curr->next;
    }
    curr->next = new;

    return 0;
}

int
as_prepare_load(struct addrspace *as)
{
    struct as_region *curr = as->region;
    while (curr != NULL) {
        curr->readable = 1;
        curr->writeable = 1;
        curr = curr->next;
    }
    return 0;
}

int
as_complete_load(struct addrspace *as)
{
    struct as_region *curr = as->region;
    while (curr != NULL) {
        curr->readable = curr->prev_read;
        curr->writeable = curr->prev_write;
        curr = curr->next;
    }
    // flush tlb incase read-write bits were set
    int spl = splhigh();
    for (int i=0; i<NUM_TLB; i++) {
        tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
    }
    splx(spl);

    return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
    // stack size defined as 16*PAGE_SIZE
    int result = as_define_region(as, USERSTACKBASE, USERSTACKSIZE, 1, 1, 0);

    if (result) {
        return result;
    }

    /* Initial user-level stack pointer */
    *stackptr = USERSTACK;

    return 0;
}


/*
**************************************************
            pagetable functions
**************************************************
*/
paddr_t *** pt_copy(paddr_t ***old_pt, int *ret) {
    vaddr_t new_frame;

    paddr_t ***new_pt = kmalloc(PT1_SIZE * sizeof(paddr_t**));
    if (new_pt == NULL) {
        *ret = ENOMEM;
        return NULL;
    }
    // null out new table:
    for (int i = 0; i < PT1_SIZE; i++) {
        new_pt[i] = NULL;
    }

    // copy in old_pt to new_pt
    for (int i = 0; i < PT1_SIZE; i++) {
        if (old_pt[i] != NULL) {
            new_pt[i] = kmalloc(PT2_SIZE * sizeof(paddr_t*));
            if (new_pt[i] == NULL) {
                *ret = ENOMEM;
                pt_destroy(new_pt);
                return NULL;
            }
            for (int j = 0; j < PT2_SIZE; j++) {
                if (old_pt[i][j] != NULL) {
                    new_pt[i][j] = kmalloc(PT3_SIZE * sizeof(paddr_t));
                    if (new_pt[i][j] == NULL) {
                        *ret = ENOMEM;
                        pt_destroy(new_pt);
                        return NULL;
                    }
                    for (int k = 0; k < PT3_SIZE; k++) {
                        if (old_pt[i][j][k] != 0) {
                            // new_pt page entry CANNOT refer to the same frame as old_pt
                            // allocate new frame in physical memory, and then create the page entry

                            // allocate physical frame:
                            new_frame = alloc_kpages(1);

                            if (new_frame == 0) {
                                /* Did not find an unallocated frame :-( */
                                *ret = ENOMEM;
                                pt_destroy(new_pt);
                                return NULL;

                            }
                            // copy data into new physical frame, borrowed from dumbmv
                            memmove((void *)new_frame,    // new_frame is already a KVADDR
                                    (const void *)PADDR_TO_KVADDR(old_pt[i][j][k] & PAGE_FRAME),
                                     PAGE_SIZE);          // no npages because we do this for each page

                            // combine frame with NDVG bits of old page
                            new_pt[i][j][k] = (KVADDR_TO_PADDR(new_frame) & PAGE_FRAME) | 
                                              (old_pt[i][j][k] & TLBLO_DIRTY) | (old_pt[i][j][k] & TLBLO_VALID);
                        } else {
                            new_pt[i][j][k] = 0;
                        }
                    }
                } else {
                    new_pt[i][j] = NULL;
                }
            }
        } else {
            new_pt[i] = NULL;
        }
    } 
    *ret = 0;
    return new_pt;

}

void pt_destroy(paddr_t ***pt) {
    if (pt == NULL) {
        return;
    }
    for (int i = 0; i < PT1_SIZE; i++) {
        if (pt[i] != NULL) {
            for (int j = 0; j < PT2_SIZE; j++) {
                if (pt[i][j] != NULL) {
                    for (int k = 0; k < PT3_SIZE; k++) {
                        if (pt[i][j][k] != 0) {
                            free_kpages(PADDR_TO_KVADDR(pt[i][j][k] & PAGE_FRAME));
                        }
                    }
                    kfree(pt[i][j]);
                }
            }
            kfree(pt[i]);
        }
    } 
    kfree(pt);
}
