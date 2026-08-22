#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"

int free_swap_slots[NSWAPSLOTS];

void swapinit() {
    for (int i = 0; i < NSWAPSLOTS; i++) {
        free_swap_slots[i] = 1;
    }
}

void mark_swap_slot_free(uint blockno) {
    int slot_index = (blockno - SWAP_SLOT_START) / 8;
    free_swap_slots[slot_index] = 1;
}

void mark_swap_slot_used(uint blockno) {
    int slot_index = (blockno - SWAP_SLOT_START) / 8;
    free_swap_slots[slot_index] = 0;
}

uint get_free_swap_slot(void) {
    for(uint i = 0; i<NSWAPSLOTS; i++){
        if(free_swap_slots[i]){
            return (8*i + SWAP_SLOT_START);
        }
    }
    panic("No free swap block");
}

pte_t* get_victim_page(struct proc *p) {
    pde_t *pde = p -> pgdir;
    for (int retries = 0; retries < 10; retries++) {
      uint sz = p->sz;
      pte_t *pte;
      for(int i = 0; i < sz; i+=PGSIZE){
        if((pte = walkpgdir(pde, (void *) i, 0)) == 0)
          panic("get_victim_page: pte should exist");
        if(!(*pte & PTE_P)) continue;
        if(!(*pte & PTE_U)) continue;
        if((*pte & PTE_A)) continue;
        return pte;
      }
      clear_proc_access_bits(p);
    }
    panic("No victim page found");
}

void swap_out_page(pte_t *page) {
    uint block_no = get_free_swap_slot();
    mark_swap_slot_used(block_no);
    move_page_memory_to_disk (ROOTDEV, (char *) P2V(PTE_ADDR(*page)), block_no);
    kfree((char *) P2V(PTE_ADDR(*page)));
    (*page) &= (~PTE_P);
    (*page) |= (PTE_SO);
    (*page) &= (0xFFF);
    (*page) |= (block_no<<(PTXSHIFT));
    return;
}

void swap_out_victim_page(void)
{
    struct proc *victim_proc = get_victim_proc();
    victim_proc -> rss -= PGSIZE;

    pte_t *victim_page = get_victim_page(victim_proc);

    swap_out_page(victim_page);
    // lcr3(V2P(victim_proc->pgdir));
}

void swap_in_page(void) {
    uint cr2 = rcr2();
    struct proc *p = myproc();
    myproc() -> rss += PGSIZE;
    pde_t * pgdir = p->pgdir;
    pte_t * pte;
    pde_t * pde = &pgdir[PDX(cr2)];
    if((*pde) & PTE_SO) {
        char * vaddr;
    if ((vaddr = (char *)kalloc())==0) 
        panic ("swap_in_page: kalloc failed");
    uint block_no = ((*pde)>> PTXSHIFT);
    move_page_disk_to_memory(ROOTDEV, vaddr, block_no);
    (*pde) &= (0xFFF);
    (*pde) |= (PTE_P);
    (*pde) &= (~PTE_SO);
    (*pde) |= (V2P((uint)vaddr));
    }
    if ( (pte = walkpgdir(pgdir, (char *) cr2, 0)) == 0)
        panic("swap_in_page: pte should exist");
    if(!((*pte) & PTE_SO)) 
        panic ("SO bit not set");
    uint block_no = ((*pte) >> PTXSHIFT);
    char * vaddr;
    if ((vaddr = (char *)kalloc())==0) 
        panic ("swap_in_page: kalloc failed");
    move_page_disk_to_memory(ROOTDEV, vaddr, block_no);
    mark_swap_slot_free(block_no);
    (*pte) &= (0xFFF);
    (*pte) |= (PTE_P);
    (*pte) &= (~PTE_SO);
    (*pte) |= (V2P((uint)vaddr));
    // lcr3(V2P(p->pgdir));
    return;
}

void print_num_used_swap_slots(void) {
    int num_blocks = myproc()->blocks_used;
    cprintf("Number of used swap slots: %d\n", num_blocks);
}