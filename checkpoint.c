#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "stat.h"

// Wire up stdin/stdout/stderr of np to the console device.  allocproc()
// returns a process with an empty ofile[] table, so a restored process has
// no way to do I/O until we attach these.  /console was created by init on
// the first boot and persists across reboots.
static void attach_console_fds(struct proc *np) {
    struct inode *cons = namei("/console");
    if (!cons) return;
    ilock(cons);
    struct file *f = filealloc();
    if (!f) { iunlockput(cons); return; }
    f->type     = FD_INODE;
    f->ip       = cons;
    f->off      = 0;
    f->readable = 1;
    f->writable = 1;
    iunlock(cons);
    np->ofile[0] = f;
    np->ofile[1] = filedup(f);
    np->ofile[2] = filedup(f);
}

// Swap out every user page of p to disk.
void swap_out_user_pages(struct proc *p) {
     uint sz = p->sz;
     pde_t* pde = p->pgdir;
      pte_t *pte;
      for(int i = 0; i < sz; i+=PGSIZE){
        if((pte = walkpgdir(pde, (void *) i, 0)) == 0)
          panic("get_victim_page: pte should exist");
        if(!(*pte & PTE_P)) continue;
        if(!(*pte & PTE_U)) continue;
        swap_out_page(pte);
        p->rss-=PGSIZE;
        p->blocks_used++;
      }
}

// Swap out every user-space page-table page to disk.
void swap_out_page_tables(struct proc *p) {
    pde_t* pde = p->pgdir;
    for(int i = 0; i<=PDX(KERNBASE) - 1; i++){
        if(!(pde[i]& PTE_P)) continue;
        if(!(pde[i] & PTE_U)) continue;
        swap_out_page((pte_t *) &pde[i]);
        // p->rss-=PGSIZE;
        p->blocks_used++;
    }

}

// Called at boot (from forkret) to restart any checkpointed processes found
// in /checkpoint. Only the trapframe/sz/name header and the list of swapped-out
// PDEs are pulled from disk here. The pgtabs and user pages come back lazily,
// from swap_in_page(), on the first page fault.
void restore_checkpoints(void) {
    struct inode *dp = namei("/checkpoint");
    if (!dp) return;
    ilock(dp);
    struct dirent de;
    for (int off = 0; off < (int)dp->size; off += sizeof(de)) {
        if (readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de)) break;
        if (de.inum == 0 || de.name[0] == '.') continue;

        char path[64] = "/checkpoint/";
        safestrcpy(path + 12, de.name, DIRSIZ);
        iunlock(dp);
        struct inode *ip = namei(path);
        ilock(dp);
        if (!ip) continue;
        ilock(ip);

        int foff = 0;
        struct proc *np = allocproc();
        np->pgdir = setupkvm();

        readi(ip, (char*)np->tf,  foff, sizeof(*np->tf)); foff += sizeof(*np->tf);
        readi(ip, (char*)&np->sz, foff, 4);               foff += 4;
        readi(ip, np->name,       foff, 16);              foff += 16;
        np->cwd = namei("/");
        attach_console_fds(np);

        uint pde_index, pgtab_slot;
        while (readi(ip, (char*)&pde_index, foff, 4) == 4) {
            foff += 4;
            readi(ip, (char*)&pgtab_slot, foff, 4); foff += 4;
            np->pgdir[pde_index] = (pgtab_slot << PTXSHIFT) | PTE_SO | PTE_W | PTE_U;
            mark_swap_slot_used(pgtab_slot);
        }

        setrunnable(np);

        iunlockput(ip);
    }
    iunlockput(dp);
}

int save_checkpoint(struct proc *p) {
    print_rss();
    print_num_used_swap_slots();
  // Step 1 (M1): swap out all user pages so every leaf PTE holds a swap slot.
  swap_out_user_pages(p);

  // Step 2 (M2): swap out the user-space page-table pages themselves so every
  // user-space PDE holds a swap slot.
  swap_out_page_tables(p);

  // Step 3: build path /checkpoint/<name>.bin
  char path[64];
  safestrcpy(path, "/checkpoint/", 13);
  int plen = 12 + strlen(p->name);
  safestrcpy(path + 12, p->name, sizeof(path) - 12);
  safestrcpy(path + plen, ".bin", sizeof(path) - plen);

  // Step 4: create the file and write the header (trapframe, sz, name).
  begin_op();
  struct inode *ip = create(path, T_FILE, 0, 0);
  if (!ip) { end_op(); return -1; }
  int off = 0;
  // Mark the saved trapframe's eax so that the restored process observes
  // checkpoint() == 1 while the original run observes 0 (the return value
  // of sys_checkpoint, which the syscall wrapper will drop into the
  // in-memory tf->eax after we return). Same idea as fork setting the
  // child's tf->eax = 0 before copying.
  p->tf->eax = 1;
  writei(ip, (char*)p->tf,  off, sizeof(*p->tf)); off += sizeof(*p->tf);
  writei(ip, (char*)&p->sz, off, 4);              off += 4;
  writei(ip, p->name,       off, 16);             off += 16;

  // Step 5: record which user-space PDEs are swapped out and their slot
  // numbers. Each record is (pde_index, pgtab_slot), 4+4 bytes.
  pde_t *pgdir = p->pgdir;
  for (int i = 0; i < PDX(KERNBASE); i++) {
    if (!(pgdir[i] & PTE_SO)) continue;
    uint pde_index   = i;
    uint pgtab_slot  = pgdir[i] >> PTXSHIFT;
    writei(ip, (char*)&pde_index,  off, 4); off += 4;
    writei(ip, (char*)&pgtab_slot, off, 4); off += 4;
  }

  iunlockput(ip);
  end_op();

  print_rss(); 
  print_num_used_swap_slots();
  return 0;
}
