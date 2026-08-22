#include "param.h"
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fs.h"
#include "fcntl.h"

// Number of 4 KB pages to allocate.
#define NPAGES 100
#define PGSZ   4096

// Pattern written to page i, byte j.
#define PATTERN(i, j) ((char)(((i) * 53 + (j)) % 251))

int
main(void)
{
  char *pages[NPAGES];

  // Allocate NPAGES pages and fill each with a deterministic pattern.
  for (int i = 0; i < NPAGES; i++) {
    pages[i] = malloc(PGSZ);
    if (pages[i] == 0) {
      printf(1, "CkTest Failed: malloc returned 0 at page %d\n", i);
      exit();
    }
    for (int j = 0; j < PGSZ; j++)
      pages[i][j] = PATTERN(i, j);
  }

  // Persist the checkpoint to disk. sys_checkpoint cprintfs the RSS
  // and number of used swap slots before and after checkpointing.
  //
  // Fork-style return value: checkpoint() returns 0 on the original
  // run and 1 on the restored run (the saved trapframe carries
  // eax=1). We use this to print a different marker in each run.
  int rv = checkpoint();

  // Re-reading every byte faults each page back in from its swap slot.
  for (int i = 0; i < NPAGES; i++) {
    for (int j = 0; j < PGSZ; j++) {
      if (pages[i][j] != PATTERN(i, j)) {
        printf(1, "CkTest Failed: page %d byte %d: got %d want %d\n",
               i, j, (int)(unsigned char)pages[i][j],
               (int)(unsigned char)PATTERN(i, j));
        exit();
      }
    }
  }

  if (rv == 0)
    printf(1, "CkTest Completed (original)\n");
  else
    printf(1, "CkTest Completed (restored)\n");
  exit();
  return 0;
}
