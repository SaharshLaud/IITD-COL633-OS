#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

struct run {
    struct run *next;
  };

// Structure for swap slots
struct swap_slot {
  int page_perm;  // Permission of swapped memory page
  int is_free;    // Availability of swap slot
};

// Array of swap slots
struct {
  struct spinlock lock;
  struct swap_slot slots[800];  // 800 swap slots as per assignment
} swap_area;

// Variables for adaptive page replacement
int threshold = 100;        // Initial threshold
int npages_to_swap = 4;     // Initial number of pages to swap (changed from 2 to 4 as per Piazza)
#ifdef ALPHA
int alpha = ALPHA;          // Alpha value from Makefile
#else
int alpha = 25;             // Default alpha value
#endif
#ifdef BETA
int beta = BETA;            // Beta value from Makefile
#else
int beta = 10;              // Default beta value
#endif
int limit = 100;            // Maximum number of pages to swap

// Initialize swap area
void
swap_init(void)
{
  initlock(&swap_area.lock, "swap_area");
  
  acquire(&swap_area.lock);
  for(int i = 0; i < 800; i++) {
    swap_area.slots[i].is_free = 1;  // Mark all slots as free initially
    swap_area.slots[i].page_perm = 0;
  }
  release(&swap_area.lock);
  
  cprintf("Swap area initialized with 800 slots\n");
}

// Find a free swap slot
int
find_free_slot(void)
{
  int i;
  
  acquire(&swap_area.lock);
  for(i = 0; i < 800; i++) {
    if(swap_area.slots[i].is_free) {
      swap_area.slots[i].is_free = 0;  // Mark as used
      release(&swap_area.lock);
      return i;
    }
  }
  release(&swap_area.lock);
  
  return -1;  // No free slot found
}

// Free a swap slot
void
free_slot(int slot_index)
{
  if(slot_index < 0 || slot_index >= 800)
    return;
    
  acquire(&swap_area.lock);
  swap_area.slots[slot_index].is_free = 1;
  swap_area.slots[slot_index].page_perm = 0;
  release(&swap_area.lock);
}

// Count free pages in memory
int
count_free_pages(void)
{
  struct run *r;
  int count = 0;
  
  extern struct {
    struct spinlock lock;
    int use_lock;
    struct run *freelist;
  } kmem;
  
  if(kmem.use_lock)
    acquire(&kmem.lock);
  r = kmem.freelist;
  while(r) {
    count++;
    r = r->next;
  }
  if(kmem.use_lock)
    release(&kmem.lock);
  
  return count;
}

// Function to swap a page out to disk
int 
swappage_out(pde_t *pgdir, uint va, uint pa) 
{
    int slot_index = find_free_slot();
    if(slot_index < 0)
        return -1;  // No free slot available
        
    // Calculate the starting block number for this slot
    uint blockno = 2 + slot_index * 8;  // 2 blocks for boot and superblock
    
    // Get the PTE for this virtual address
    pte_t *pte = walkpgdir(pgdir, (void*)va, 0);
    if(!pte || !(*pte & PTE_P))
        return -1;  // Page not present
        
    // Save the page permissions
    acquire(&swap_area.lock);
    swap_area.slots[slot_index].page_perm = *pte & 0xFFF;  // Save the lower 12 bits (flags)
    release(&swap_area.lock);
    
    // Write the page to disk
    for(int i = 0; i < 8; i++) {
        struct buf *b = bread(0, blockno + i);
        memmove(b->data, (char*)(P2V(pa)) + i*BSIZE, BSIZE);
        bwrite(b);
        brelse(b);
    }
    
    // Update the PTE to point to the swap slot
    // Clear the PTE_P bit and set the slot index in the PPN field
    // Make sure to preserve the user and write permissions
    *pte = (slot_index << 12) | ((*pte) & ~PTE_P & 0xFFF);
    
    // Flush the TLB
    lcr3(V2P(pgdir));
    
    //cprintf("Swapped out page at VA 0x%x to slot %d\n", va, slot_index);
    return 0;
}


// Function to swap a page in from disk
int 
swappage_in(uint va) 
{
    struct proc *p = myproc();
    if(!p)
        return -1;
    
    // Round down to page boundary
    uint page_addr = PGROUNDDOWN(va);
    
    pde_t *pgdir = p->pgdir;
    pte_t *pte = walkpgdir(pgdir, (void*)page_addr, 0);
    
    if(!pte) {
       // cprintf("No PTE for address 0x%x\n", va);
        return -1;  // No PTE for this address
    }
    
    if(*pte & PTE_P) {
       // cprintf("Page already present for address 0x%x\n", va);
        return -1;  // Page already present
    }
    
    // Extract the slot index from the PTE
    // Use PTE_ADDR to get the physical address bits (top 20 bits)
    int slot_index = PTE_ADDR(*pte) >> 12;
    
    //cprintf("Extracted slot index %d for address 0x%x\n", slot_index, va);
    
    if(slot_index < 0 || slot_index >= 800 || swap_area.slots[slot_index].is_free) {
       // cprintf("Invalid swap slot: %d (va: 0x%x)\n", slot_index, va);
        return -1;  // Invalid slot or slot is free
    }
    
    // Allocate a new physical page
    char *mem = kalloc();
    if(!mem) {
        // Out of memory - try to free up some space by swapping
        check_and_swap();
        mem = kalloc();  // Try again
        if(!mem) {
          //  cprintf("Still out of memory after swapping\n");
            return -1;  // Still out of memory
        }
    }
    
    // Calculate the starting block number for this slot
    uint blockno = 2 + slot_index * 8;  // 2 blocks for boot and superblock
    
    // Read the page from disk
    for(int i = 0; i < 8; i++) {
        struct buf *b = bread(0, blockno + i);
        memmove(mem + i*BSIZE, b->data, BSIZE);
        brelse(b);
    }
    
    // Restore the page permissions
    uint perm;
    acquire(&swap_area.lock);
    perm = swap_area.slots[slot_index].page_perm;
    release(&swap_area.lock);
    
    // Make sure PTE_P is set in the permissions
    perm |= PTE_P;
    
    // Map the physical page to the virtual address
    if(mappages(pgdir, (void*)page_addr, PGSIZE, V2P(mem), perm) < 0) {
        kfree(mem);
       // cprintf("Failed to map page at VA 0x%x\n", page_addr);
        return -1;
    }
    
    // Free the swap slot
    free_slot(slot_index);
    
    // Increment the rss count
    p->rss++;
    
 //   cprintf("Successfully swapped in page at VA 0x%x from slot %d\n", page_addr, slot_index);
    return 0;
}


// Function to find a victim process for swapping
struct proc*
find_victim_proc(void)
{
  struct proc *p;
  struct proc *victim = 0;
  int max_rss = 0;  // Changed from -1 to 0
  
  extern struct {
    struct spinlock lock;
    struct proc proc[NPROC];
  } ptable;
  
  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if(p->state == UNUSED || p->pid < 1)
      continue;
    
    // Add debug output to see rss values
   // cprintf("Process %d has rss %d\n", p->pid, p->rss);
    
    if(p->rss > max_rss || (p->rss == max_rss && victim && p->pid < victim->pid)) {
      max_rss = p->rss;
      victim = p;
    }
  }
  release(&ptable.lock);
  
  //if(victim)
   // cprintf("Selected victim process %d with rss %d\n", victim->pid, victim->rss);
  //else
   // cprintf("No victim process found with non-zero rss\n");
  
  return victim;
}



// Function to find a victim page in a process
uint 
find_victim_page(pde_t *pgdir, uint *va_out) 
{
  uint i, pa;
  pte_t *pte;
  
  // First pass: look for pages with PTE_A unset
  for(i = 0; i < KERNBASE; i += PGSIZE) {
    pte = walkpgdir(pgdir, (void*)i, 0);
    if(!pte || !(*pte & PTE_P) || !(*pte & PTE_U))
      continue;
      
    if(!(*pte & PTE_A)) {
      // Found a page with PTE_P set and PTE_A unset
      pa = PTE_ADDR(*pte);
      *va_out = i;
      return pa;
    }
  }
  
  // If no page with PTE_A unset is found, reset PTE_A for all pages and try again
  for(i = 0; i < KERNBASE; i += PGSIZE) {
    pte = walkpgdir(pgdir, (void*)i, 0);
    if(!pte || !(*pte & PTE_P) || !(*pte & PTE_U))
      continue;
      
    // Reset PTE_A for all pages
    *pte &= ~PTE_A;
  }
  
  // Flush TLB after modifying page table entries
  lcr3(V2P(pgdir));
  
  // Second pass: now all pages have PTE_A unset, so pick the first one
  for(i = 0; i < KERNBASE; i += PGSIZE) {
    pte = walkpgdir(pgdir, (void*)i, 0);
    if(!pte || !(*pte & PTE_P) || !(*pte & PTE_U))
      continue;
      
    // Found a page with PTE_P set (PTE_A is now unset for all pages)
    pa = PTE_ADDR(*pte);
    *va_out = i;
    return pa;
  }
  
  return 0;  // No suitable page found (should not reach here)
}




// Function to swap out pages based on the adaptive policy
void 
swap_out_pages(void) 
{
  struct proc *victim = find_victim_proc();
  if(!victim) {
   // cprintf("No victim process found for swapping\n");
    return;
  }
  
 // cprintf("Selected victim process %d with %d pages\n", victim->pid, victim->rss);
  
  int swapped = 0;
  int attempts = 0;
  while(swapped < npages_to_swap && attempts < npages_to_swap * 2) {
    uint va;
    uint pa = find_victim_page(victim->pgdir, &va);
    if(pa == 0) {
    //  cprintf("No suitable page found for swapping\n");
      break;  // No suitable page found
    }
    
    if(swappage_out(victim->pgdir, va, pa) == 0) {
      // Successfully swapped out the page
      victim->rss--;
      kfree((char*)P2V(pa));  // Free the physical page
      swapped++;
     // cprintf("Swapped out page at VA 0x%x, PA 0x%x\n", va, pa);
    } else {
    //  cprintf("Failed to swap out page at VA 0x%x, PA 0x%x\n", va, pa);
    }
    attempts++;
  }
  
 // cprintf("Swapped %d pages after %d attempts\n", swapped, attempts);
}


// Adaptive page replacement function
void
check_and_swap(void)
{
  int free_pages = count_free_pages();
  
  if(free_pages <= threshold) {
    //cprintf("Current Threshold = %d, Swapping %d pages (Free pages: %d)\n",threshold, npages_to_swap, free_pages);
    cprintf("Current Threshold = %d, Swapping %d pages\n",threshold, npages_to_swap);

    
    // Swap out npages_to_swap pages
    swap_out_pages();
    
    // Update threshold and npages_to_swap
    threshold = (threshold * (100 - beta)) / 100;
    if(threshold < 1) threshold = 1;  // Ensure threshold doesn't go below 1
    
    npages_to_swap = ((npages_to_swap * (100 + alpha)) / 100);
    if(npages_to_swap > limit)
      npages_to_swap = limit;
    
    // Print free pages after swapping
   // cprintf("After swapping, free pages: %d\n", count_free_pages());
  }
}


// Clean up swap slots for a process when it exits
void
swap_cleanup(struct proc *p)
{
  pte_t *pte;
  uint i;
  
  if(!p || !p->pgdir)
    return;
    
  for(i = 0; i < KERNBASE; i += PGSIZE) {
    pte = walkpgdir(p->pgdir, (void*)i, 0);
    if(!pte)
      continue;
      
    if(!(*pte & PTE_P) && (*pte != 0)) {
      // This is a swapped-out page
      int slot_index = *pte >> 12;
      if(slot_index >= 0 && slot_index < 800) {
        free_slot(slot_index);
      }
    }
  }
}
