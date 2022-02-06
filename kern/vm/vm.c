#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>
#include <machine/tlb.h>
#include <spl.h>
#include <proc.h>
#include <current.h>

/* Place your page table functions here */


void vm_bootstrap(void)
{
    /* Initialise any global components of your VM sub-system here.  
     *  
     * You may or may not need to add anything here depending what's
     * provided or required by the assignment spec.
     */
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
    // for inserting into TLB:
    uint32_t ehi = 0;
    uint32_t elo = 0;

    //uint32_t n = 0; <- not supported
    uint32_t d = TLBLO_DIRTY;
    //uint32_t g = 0; <- not implementing ASID
    uint32_t v = TLBLO_VALID;

    // split virtual address:
    uint32_t index1 = (faultaddress & PT1_INDEX) >> 24;
    uint32_t index2 = (faultaddress & PT2_INDEX) >> 18;
    uint32_t index3 = (faultaddress & PT3_INDEX) >> 12;
    //uint32_t offset = faultaddress & PT_OFFSET; <- unused

    struct addrspace *as;
    int spl;

    // preliminary checks taken from dumbvm.c:

    // VM_FAULT READONLY?
    switch (faulttype) {
        case VM_FAULT_READONLY:
            return EFAULT;
        case VM_FAULT_READ:
        case VM_FAULT_WRITE:
            break;
        default:
            return EINVAL;
    }

    // check curproc
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

    // assert address space has been set up properly? <- dunno if needed

    /********************/
    /*                  */
    /* Lookup PageTable */
    /*                  */
    /********************/

    // get page table
    paddr_t ***pt = as->pagetable;
    if (pt == NULL) {
        return EFAULT;
    }

    // Valid Translation?

    if (pt[index1] != NULL) if (pt[index1][index2] != NULL) if (pt[index1][index2][index3] != 0) {
    // Yes: Load TLB
        ehi = faultaddress & TLBHI_VPAGE; // not implementing ASID in asst3
        elo = pt[index1][index2][index3]; // we made page entries = entrylo format for easy insert
        
        /* Disable interrupts on this CPU while frobbing the TLB. */
        spl = splhigh();
        tlb_random(ehi, elo);
        splx(spl);
		return 0;
    }

    // No:  Look up Region

	struct as_region *curr = as->region;
    bool valid_region = false;

	// check for if faultaddress is within a region's vaddr range
    // modified from as_define_region()
    vaddr_t bot;
    vaddr_t top;
	while (curr != NULL) {
		bot = curr->vbase;
		top = bot + PAGE_SIZE * curr->npages;
		if (faultaddress >= bot && faultaddress < top) {
            // faultaddress is within a valid region!
            if (curr->writeable == 0) {
                d = 0; // set dirty bit for entrylo
                if (curr->readable == 0 && curr->executable == 0) {
                    v = 0; // not valid if no permissions at all
                }
            }
            valid_region = true;
            break;
		}
		curr = curr->next;
	}

    // Valid Region?

    if (valid_region == false) {
    // No: EFAULT
        return EFAULT;
    }

    // Yes: allocate a physical frame for requested vaddr


    // allocate deeper pagetable levels if needed
    // level 2:
    if (pt[index1] == NULL) {
        pt[index1] = kmalloc(PT2_SIZE * sizeof(paddr_t*));
        if (pt[index1] == NULL) {
            return ENOMEM;
        }
        for (int j = 0; j < PT2_SIZE; j++) {
            pt[index1][j] = NULL;
        }
    }
    // level 3:
    if (pt[index1][index2] == NULL) {  // if 2nd lvl doesnt point to a lvl 3 table:
        pt[index1][index2] = kmalloc(PT3_SIZE * sizeof(paddr_t));
        if (as->pagetable[index1][index2] == NULL) {
            return ENOMEM;
        }
        for (int k = 0; k < PT3_SIZE; k++) {
            as->pagetable[index1][index2][k] = 0;
        }
    }

    // allocate frame
    vaddr_t new_frame = alloc_kpages(1); // not sure if alloc_one_frame or alloc_kpages
    if (new_frame == 0) {
        /* Did not find an unallocated frame :-( */
        return ENOMEM;
    }
    // zero out the physical memory upon allocation
    bzero((void*)new_frame, PAGE_SIZE);

    // combine frame with NDVG bits to form a page
    // insert PTE
    pt[index1][index2][index3] = (KVADDR_TO_PADDR(new_frame) & PAGE_FRAME) | d | v;
    
    // zero fill newly allocated user-level page
    // ^ level 3 already zero filled

    // finally:
    // load TLB
    ehi = faultaddress & TLBHI_VPAGE; // not implementing ASID in asst3
    elo = pt[index1][index2][index3]; // we made page entries = entrylo format for easy insert
    
    /* Disable interrupts on this CPU while frobbing the TLB. */
    spl = splhigh();
    tlb_random(ehi, elo);
    splx(spl);

    return 0;
}

/*
 * SMP-specific functions.  Unused in our UNSW configuration.
 */

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("vm tried to do tlb shootdown?!\n");
}

