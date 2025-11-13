#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[], end[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

int cow();
int ldvma(uint64 va);

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void)
{
  int which_dev = 0;

  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();
  
  // save user program counter.
  p->trapframe->epc = r_sepc();
  
  if(r_scause() == 8){
    // system call

    if(p->killed)
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sstatus &c registers,
    // so don't enable until done with those registers.
    intr_on();

    syscall();
  } else if((r_scause()==12) || (r_scause()==13) || (r_scause()==15)) {
    cow();
  } else if((which_dev = devintr()) != 0){
    // ok
  } else {
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->killed = 1;
  }

  if(p->killed)
    exit(-1);

  // timer interrupt.
  if(which_dev == 2) {
    p->tickspassed += 1;
    if ( (p->tickspassed == p->alarmintvl) && (p->alarmlock == 0) ) {
      p->alarmlock = 1; // indicate alarm handler in progress
      // save original registers and states to a scratch frame
      memmove(p->alarmfr, p->trapframe, sizeof(struct trapframe));

      p->tickspassed = 0;
      p->trapframe->epc = p->alarmhdlr;
    }
    yield();
  }

  usertrapret();
}

//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  intr_off();

  // send syscalls, interrupts, and exceptions to trampoline.S
  w_stvec(TRAMPOLINE + (uservec - trampoline));

  // set up trapframe values that uservec will need when
  // the process next re-enters the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to.
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 fn = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64,uint64))fn)(TRAPFRAME, satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    printf("scause %p\n", scause);
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if((scause & 0x8000000000000000L) &&
     (scause & 0xff) == 9){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr();
    } else if(irq == E1000_IRQ){
      e1000_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000001L){
    // software interrupt from a machine-mode timer interrupt,
    // forwarded by timervec in kernelvec.S.

    if(cpuid() == 0){
      clockintr();
    }
    
    // acknowledge the software interrupt by clearing
    // the SSIP bit in sip.
    w_sip(r_sip() & ~2);

    return 2;
  } else {
    return 0;
  }
}

// handle a page fault from copy-on-write
// create new page 
// return -1 if unsuccessful
int
cow()
{
  uint64 va, pa; 
  pte_t *pte;
  uint flags;
  char *mem;
  struct proc *p = myproc();

  va = r_stval();
  if (va >= MAXVA) {
    p->killed = 1; // kill offending proccess
    return -1; 
  }

  va = PGROUNDDOWN(va);
  if ((pte = walk(p->pagetable, va, 0)) == 0) {
    printf("page fault: va not in pgtbl\n");
    p->killed = 1;
    return -1;
  }

  // check if it is not copy-on-write
  if ((*pte & PTE_C) == 0) {
    // check if it is not vma loading
    if (ldvma(va) == -1) {
      printf("Page Fault.\n");
      p->killed = 1; // kill on actual page fault
      return -1;
    } else {
      return 0;
    }
  }

  // create new physical page
  if ((mem = kalloc()) == 0) {
    p->killed = 1; // kill proccess if no physical mem
    return -1;
  }
  pa = PTE2PA(*pte);
  memmove(mem, (char*)pa, PGSIZE);

  // set write permission bits
  flags = PTE_FLAGS(*pte);
  flags = flags & (~PTE_C);
  flags = flags | PTE_W;

  // remove old mapping
  uvmunmap(p->pagetable, va, 1, 1);

  // add new mapping
  if (mappages(p->pagetable, va, PGSIZE, (uint64)mem, flags) != 0) {
    panic("Copy-on-write page mapping failed");
  }

  // re-execute faulting instruction
  // keep p->trapframe->epc unchanged
  return 0;
}

// load virtual memory area page
// return -1 if the given virtual address is not in VMA
int
ldvma(uint64 va)
{
  struct proc *p = myproc();
  char *mem;
  int i;

  // find which VMA 
  for (i = 0; i < NVMA; i++) {
    if (p->vma_areas[i].addr == 0) {
      continue;
    }
    if (va >= p->vma_areas[i].addr && va < p->vma_areas[i].addr + p->vma_areas[i].length) {
      break;
    }
  }

  // va not in any VMA
  if (i == NVMA) {
    printf("faulting virtual address: %p\n", va);
    return -1;
  }

  struct inode *ip = p->vma_areas[i].f->ip;
  int off = p->vma_areas[i].offset;
  uint64 addr = p->vma_areas[i].addr;
  uint64 uaddr = PGROUNDDOWN(va);
  int perm = PTE_V | PTE_U | p->vma_areas[i].perm << 1;

  // create physical page
  if ((mem = kalloc()) == 0) {
    p->killed = 1;
    return -1;
  }

  // create new mapping
  if (mappages(p->pagetable, uaddr, PGSIZE, (uint64)mem, perm) != 0) {
    panic("Create VMA mapping failed");
  }

  // load file content into user address
  int fileoff = (va - addr) + off;
  ilock(ip);
  int n = readi(ip, 1, uaddr, fileoff, PGSIZE);
  iunlock(ip);
  memset(mem + n, 0, PGSIZE - n);
  return 0;
}
