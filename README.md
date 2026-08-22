# COL331 Lab4: Swap Space

As processes consume more memory, the system can run out of physical memory. To
mitigate this issue, this lab aims to add support for *swap space* in xv6. This
swap space on disk stores memory pages, freeing up memory for other processes.

## What is Already Provided

The starter code includes significant infrastructure to support page swapping.
You should study these changes carefully before writing any code.

### Disk Organization (provided)

The disk layout has been modified from
`[ boot block | sb block | log | inode blocks | free bit map | data blocks ]` to
`[ boot block | sb block | swap blocks | log | inode blocks | free bit map | data blocks ]`.
A *swap blocks* partition has been added between the `sb block` and `log`.

- `mkfs.c`: Sets up the new disk layout. The superblock now contains
`nswapspaceblocks` and `swapstart` fields.
- `fs.h`: The `struct superblock` has been augmented with `nswapspaceblocks`
(total disk blocks in swap space) and `swapstart` (block number of the first
swap block).
- `param.h`: Defines `SWAP_SLOT_START` (=2, the starting block of swap
space on disk), `NSWAPSLOTS` (=300, the number of swap slots), and `FSSIZE`
(=3500).

Each swap slot consists of 8 consecutive disk blocks (since a page is 4096 bytes
and a disk block is 512 bytes: 4096/512 = 8). The swap region therefore occupies
`NSWAPSLOTS * 8 = 2400` disk blocks starting at block 2.

### Page I/O Helpers (provided)

`bio.c` provides two functions for moving page data between memory and disk:

- `move_page_memory_to_disk(int dev, char *v_addr, uint blockno)` - writes the  
contents of a page at virtual address `v_addr` to 8 consecutive disk blocks
starting at `blockno`.
- `move_page_disk_to_memory(int dev, char *v_addr, uint blockno)` - reads 8  
consecutive disk blocks starting at `blockno` into the page at virtual address
`v_addr`.

These bypass the log layer since they store volatile memory pages.

### RSS Tracking (provided)

Each process now tracks its **resident set size** (RSS) - the amount of  
physical memory it currently occupies:

- `proc.h`: `struct proc` has a new `uint rss` field (in bytes).
- `proc.c`: `allocproc()` initializes `rss` to `PGSIZE` (for the kernel
stack).
- `vm.c`: `allocuvm()` increments `rss` by `PGSIZE` for each page
allocated. `deallocuvm()` decrements `rss` by `PGSIZE` for each present page
freed, and also frees swap slots for any swapped-out pages via
`mark_swap_slot_free()`. `copyuvm()` increments the child's `rss` for each
page copied.

**Note:** Do not make any changes to how rss is being managed otherwise your
code may fail the testcases

### Victim Process Selection (provided)

`proc.c` provides `get_victim_proc()`, which selects the process with the
highest `rss` value. If two processes have the same `rss`, the one with the
lower `pid` is chosen.

### Access Bit Clearing (provided)

`vm.c` provides `clear_proc_access_bits(struct proc *p)`, which clears the
`PTE_A` (accessed) flag on up to 10 user pages of process `p`. This is used
during victim page selection when no non-accessed page can be found.

### Integration Points (provided)

- `kalloc.c`: When `kalloc()` fails to find a free page, it calls
`swap_out_victim_page()` which calls `swap_out_page()` (your function) and
retries.
- `trap.c`: When a `T_PGFLT` (page fault) trap occurs, it calls
`swap_in_page()` (your function).
- `defs.h`: Declares `swap_out_victim_page()`, `swap_out_page()`,
`swap_in_page()`, `swapinit()`, `mark_swap_slot_free()` and
`mark_swap_slot_used()`.

### PTE Flags (provided in `mmu.h`)

The following page table entry flags are relevant:

- `PTE_P` (0x001) - Present: the page is in physical memory.
- `PTE_W` (0x002) - Writeable.
- `PTE_U` (0x004) - User-accessible.
- `PTE_SO` (0x008) - Swapped Out: custom flag indicating the page resides in  
swap space.
- `PTE_A` (0x020) - Accessed: set by the x86 hardware when the page is read or  
written.
- `PTE_ADDR(pte)` - extracts the upper 20 bits (the physical page frame or,  
for swapped-out pages, the swap block number).
- `PTE_FLAGS(pte)` - extracts the lower 12 bits (the flags).
- `PTXSHIFT` (=12) - the bit shift for page table indexing.

### Swap Slot Tracking (provided in `pageswap.c`)

The starter code in `pageswap.c` provides:

- `int free_swap_slots[NSWAPSLOTS]` - an array tracking which swap slots are  
free (1 = free, 0 = occupied).
- `swapinit()` - initializes all swap slots to free. Called once during boot  
from `forkret()`.
- `mark_swap_slot_free(uint blockno)` - given a starting block number, marks  
the corresponding swap slot as free.
- `mark_swap_slot_used(uint blockno)` - given a starting block number, marks  
the corresponding swap slot as used.

### Useful Functions

- `walkpgdir(pde_t *pgdir, const void *va, int alloc)` - returns a pointer to  
the page table entry for virtual address `va`. If `alloc` is 0, it does not
allocate new page tables. (Defined in `vm.c`.)
- `rcr2()` - reads the `cr2` register, which holds the faulting virtual address
after a page fault. (Defined in `x86.h`.)
- `kalloc()` / `kfree(char *v)`- allocate/free a physical page.
- `P2V(pa)` / `V2P(va)` - convert between physical and virtual (kernel)  
addresses.

---

## What You Need to Implement

You need to implement **three functions** in `pageswap.c`:

### 1. `pte_t* get_victim_page(struct proc *p)`

Given a victim process `p`, find a victim page to evict. Iterate through the
page directory and page tables of `p` and choose a page with the `PTE_P` flag
set and the `PTE_A` flag unset. The `PTE_P` flag indicates whether the page is
present in the memory, and the `PTE_A` flag indicates whether the page has been
accessed by the process, which is set by the x86 paging hardware (refer Ch-2 of
the xv6 book). Only consider user pages (`PTE_U` set).

If no such page is found, use `clear_proc_access_bits(p)` to convert some
accessed pages to non-accessed and try again. Repeat for a reasonable number of
retries before panicking.

### 2. `void swap_out_page(pte_t* page)`

This function is called by `swap_out_victim_page()` which selects a victim page.
`swap_out_victim_page()` is called inside `kalloc()` when no free physical pages
are available.  Select a victim process (using `get_victim_proc()`), then find a
victim page from it. Write the victim page's contents into an available swap
slot on disk, free the physical page, and update the page table entry of the
swapped-out page so that the swap location can be recovered later.

### 3. `void swap_in_page(void)`

This function is called from `trap.c` when a page fault (`T_PGFLT`) occurs
because a process accessed a swapped-out page. Read the faulting virtual address
from the `cr2` register, locate the corresponding PTE, and determine where the
page was stored on disk. Allocate a new physical page, copy the data back from
disk to memory, restore the page table entry, and free the swap slot.

You are free to add more functions as required by your implementation.

---

## Deliverables

Run the commands provided below to create a tarball with the name, `lab4.tar.gz`
in the same directory.  Submit this tarball on Moodle.  

```bash
make clean
tar czvf lab4.tar.gz * 
```

## Grading Rules

Files you should not make changes to: `memlayout.h, mmu.h`.

You should not make any changes to the following system calls: `getrss` and
`getNumFreePages`

You can manually test the testcases by running `make qemu` and then type in the
xv6 shell - `$ memtest1` to test the first testcase and `$ memtest2` to test the
second testcase

There are two test cases for this assignment. Currently both the testcases would
fail due to lack of memory. We are including the outputs with some comments.

- memtest1: `MemTest1 Passed!`
- memtest2:

```txt
Parent alloc-ed:
PrintingRSS
((P)) id: 1, state: 2, rss: 12288
((P)) id: 2, state: 2, rss: 20480
((P)) id: 3, state: 4, rss: 1433600
((P)) id: 4, state: 3, rss: 974848
Child alloc-ed
PrintingRSS
((P)) id: 1, state: 2, rss: 12288
((P)) id: 2, state: 2, rss: 20480
((P)) id: 3, state: 2, rss: 1392640
((P)) id: 4, state: 4, rss: 1400832
Memtest2 Passed!
```

Here the resident size of the parent process should decrease as shown in the
above output (for process with pid 3).

Kindly also note that we use a different version of `init.c` in the autograder
hence the rss values differ slightly compared to the ones which appear
when the test is run manually (given above).

## Auto-grader

Your submission will be evaluated by our autograder.
Follow the below instructions to run the autograder locally on your submission:

- Download and unzip the file lab4_check_scripts.zip.

```bash
unzip lab4_check_scripts.zip
cd lab4_check_scripts 
```

- Use the following command to run the auto-grader

```bash
bash check.sh /path/to/lab4.tar.gz
```
