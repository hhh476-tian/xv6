// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);
int getcpuid();

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];

uint64 refcounts[PHYSTOP / PGSIZE];

void
kinit()
{
  for (int i = 0; i < NCPU; i++) {
    initlock(&kmem[i].lock, "kmem");
  }
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE) {
    refcounts[PGREF(p)] = 0;
    kfree(p);
  }
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
  int i;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Do not free with multiple refcounts
  if (refcounts[PGREF(pa)] > 1) {
    kdecref((uint64)pa); // decrease ref count
    return;
  }

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  // get lock per cpu
  i = getcpuid();

  acquire(&kmem[i].lock);
  r->next = kmem[i].freelist;
  kmem[i].freelist = r;
  release(&kmem[i].lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  int i;

  i = getcpuid();

  acquire(&kmem[i].lock);
  r = kmem[i].freelist;
  if(r) {
    kmem[i].freelist = r->next;
  } else {
    // steal memory from other cpu
    for (int j = 0; j < NCPU; j++) {
      if (i == j) {
        continue;
      }
      acquire(&kmem[j].lock);
      r = kmem[j].freelist;
      if(r) {
        kmem[j].freelist = r->next;
        release(&kmem[j].lock);
        break;
      }
      release(&kmem[j].lock);
    }
  }
  refcounts[PGREF(r)] = 1;
  release(&kmem[i].lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

// Get amount of free memory in bytes
uint64
kgetfree(void)
{
  uint64 num = 0;
  struct run *r;
  int i;

  i = getcpuid();
  
  r = kmem[i].freelist;
  while (r) {
    num++;
    r = r->next;
  }
  return num*PGSIZE;
}

void
kdecref(uint64 pa) {
  int i;

  i = getcpuid();
  acquire(&kmem[i].lock);
  refcounts[PGREF(pa)] -= 1;
  release(&kmem[i].lock);
}

void
kincref(uint64 pa) {
  int i;

  i = getcpuid();
  acquire(&kmem[i].lock);
  refcounts[PGREF(pa)] += 1;
  release(&kmem[i].lock);
}

// safely get cpu id with interrupts turnned off
int getcpuid() {
  int i;

  push_off();
  i = cpuid();
  pop_off();

  return i;
}
