#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "mm.h"
#include "param.h"
#include "memlayout.h"
#include "sysinfo.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "proc.h"
#include "fcntl.h"

uint64 findregion(uint64 size);
int invma(uint64 addr);
void clear_vma();

uint64
sys_mmap(void)
{
  int length;
  int prot;
  int flags;
  int fd;
  struct file *f;
  int offset = 0;
  uint64 addr;
  struct proc *p = myproc();

  // read arugment 
  if (argint(1, &length) < 0 || argint(2, &prot) || argint(3, &flags)
        || argint(4, &fd) || argint(5, &offset)) {
    printf("bad file\n");
    return -1;
  }

  // get file from descriptor
  if(fd < 0 || fd >= NOFILE || (f=p->ofile[fd]) == 0) {
    printf("bad file\n");
    return -1;
  }
    
  if (!(f->type == FD_INODE)) {
    printf("bad file type\n");
    return -1;
  }

  // check permissions
  if (!f->writable && (prot & PROT_WRITE) && (flags & MAP_SHARED)) {
    printf("bad perm\n");
    return -1;
  }

  // find an unused region
  acquire(&p->vma_lock);
  addr = findregion(length);

  // add to process's VMA
  int i;
  for (i = 0; i < NVMA; i++) {
    if (p->vma_areas[i].addr == 0) {
        p->vma_areas[i].addr = addr;
        p->vma_areas[i].length = length;
        p->vma_areas[i].perm = prot;
        p->vma_areas[i].f = f;
        p->vma_areas[i].offset = offset;
        p->vma_areas[i].flags = flags;
        break;
    }
  }
  release(&p->vma_lock);

  if (i == NVMA) {
    return -1;
  }

  // increase file ref count
  filedup(f);
  
  return addr;
}

uint64
sys_munmap(void)
{
  // read argument
  int length;
  uint64 addr;
  struct proc *p = myproc();
  int i;

  if (argaddr(0, &addr) < 0 || argint(1, &length))
    return -1;

  if (length % PGSIZE) {
    return -1; // length must be page size aligned
  }

  // find which VMA 
  i = invma(addr);
  if (i == -1) {
    printf("unmap: unknown VMA region: %p\n", addr);
    return -1;
  }

  // write back dirty pages
  if (p->vma_areas[i].flags & MAP_SHARED) {
    filewrite(p->vma_areas[i].f, addr, length);
  }

  // unmap the region
  uint64 start = addr;
  int npages = length / PGSIZE;
  uint64 a;
  pte_t *pte;

  for (a = start; a < start + npages*PGSIZE; a = a + PGSIZE) {
    pte = walk(p->pagetable, a, 0);
    if (*pte & PTE_V) {
      uvmunmap(p->pagetable, start, npages, 1);
    }
  }
  
  // decrease file ref count if all region unmapped
  if (length == p->vma_areas[i].length) {
    filededup(p->vma_areas[i].f);
    memset(&p->vma_areas[i], 0, sizeof(p->vma_areas[i])); // clear VMA
  } else {
    // update VMA info if some region got unmapped
    p->vma_areas[i].addr += length;
    p->vma_areas[i].length -= length;
  }

  return 0;
}

// find a free virtual memory regeion
// of size at least n pages
// return the start of the virtal
uint64
findregion(uint64 size) 
{
  uint64 a;
  pte_t *pte;
  struct proc *p = myproc();
  pagetable_t pagetable = p->pagetable;
  uint64 start = PGROUNDUP(p->sz);
  uint64 limit = MAXVA - 2*PGSIZE;
  uint64 max_start = limit - size;

  size = PGROUNDUP(size);
  if(size == 0)
    return 0;

  while(start <= max_start){
    // printf("start: %p\n", start);
    for(a = start; a < start + size; a += PGSIZE){
      if (invma(a) != -1)
        break;
      pte = walk(pagetable, a, 0);
      if(pte != 0 && (*pte & PTE_V))
        break;
    }
    if(a >= start + size)
      return start;

    start = PGROUNDUP(a + PGSIZE);
  }

  return 0;
}

// check if a given address is in process's VMA table
// return the index in VMA table if found 
// otherwise return -1
int
invma(uint64 addr) 
{
  int i;
  struct proc *p = myproc();

  for (i = 0; i < NVMA; i++) {
    if (p->vma_areas[i].addr == 0) {
      continue;
    }
    if (addr >= p->vma_areas[i].addr && addr < p->vma_areas[i].addr + p->vma_areas[i].length) {
      return i;
    }
  }

  return -1;
}

// clear all VMA
void
clear_vma() 
{
  int i;
  uint64 a, start;
  pte_t *pte;
  struct proc *p = myproc();

  for (i = 0; i < NVMA; i++) {
    start = p->vma_areas[i].addr;
    if (!start) {
        continue;
    }

    for (a = start; a < start + p->vma_areas[i].length; a = a + PGSIZE) {
        pte = walk(p->pagetable, a, 0);
        if (*pte & PTE_V) {
            uvmunmap(p->pagetable, a, 1, 1);
        }
    }
  }
}
