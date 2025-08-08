#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sysinfo.h"

uint64
sys_exit(void)
{
  int n;
  if(argint(0, &n) < 0)
    return -1;
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  if(argaddr(0, &p) < 0)
    return -1;
  return wait(p);
}

uint64
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  // backtrace();

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_trace(void)
{
  int num;
  if(argint(0, &num) < 0)
    return -1;
  myproc()->tracemask = num;
  return 0;
}

uint64
sys_sysinfo(void)
{
  struct sysinfo s;
  uint64 p; // user address for struct sysinfo*

  if(argaddr(0, &p) < 0)
    return -1;

  s.freemem = kgetfree();
  s.nproc = numproc();

  // copy struct sysinfo from kernel to user address
  pagetable_t pagetable = myproc()->pagetable;
  if(copyout(pagetable, p, (char *)(&s), sizeof(s))) 
    return -1;
  return 0;
}

uint64
sys_pgaccess(void)
{
  uint64 va, umask;
  int len;
  argaddr(0, &va); // start of virtual address to check
  argint(1, &len); // number of pages to check
  argaddr(2, &umask); // get user address for the bist mask

  uint64 bitmask = 0; 
  int maxpages = 32; // can only check 32 pages at max
  if (len > maxpages) {
    return -1;
  }

  pte_t* p;
  for (int i = 0; i < len; i++) {
    p = walk(myproc()->pagetable, va + i*PGSIZE, 0);
    uint64 pte = *p;
    if (pte & PTE_A) {
      bitmask = bitmask | (1 << i);
      *p = pte & (~PTE_A); // clear PTE_A bit
    }
  }

  copyout(myproc()->pagetable, umask, (char*)(&bitmask), (int)(len/4));
  return 0;
}

uint64
sys_sigalarm(void)
{
  int interval;
  uint64 handler;

  argint(0, &interval);
  argaddr(1, &handler);

  myproc()->alarmintvl = interval;
  myproc()->alarmhdlr = handler;
  myproc()->tickspassed = 0;
  // store a scratch trapframe below actual trapframe
  myproc()->alarmfr = (struct trapframe *)(TRAPFRAME - sizeof(struct trapframe)); 
  myproc()->alarmlock = 0;

  return 0;
}

uint64
sys_sigreturn(void)
{
  // restore registers and states
  struct proc *p = myproc();
  memmove(p->trapframe, p->alarmfr, sizeof(struct trapframe));
  p->alarmlock = 0;
  return 0;
}
