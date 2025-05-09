#include "types.h"
#include "riscv.h"
#include "param.h"
#include "defs.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sysinfo.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
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
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
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


  argint(0, &n);
  acquire(&tickslock);
  ticks0 = ticks;
#ifdef LAB_TRAPS
  backtrace();
#endif
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}


#ifdef LAB_PGTBL
int
sys_pgaccess(void)
{
  uint64 addr;
  int n;
  uint64 buf;
  struct proc *p = myproc();
  argaddr(0, &addr);
  argint(1, &n);
  argaddr(2, &buf);
  if (n < 0 || n > 32)
    return -1;

  uint64 mask = 0;
  for(int i = 0; i < n; i++){
    pte_t *pte = walk(p->pagetable, addr + i * PGSIZE, 0);
    if(pte == 0)
      return -1;
    if(*pte & PTE_A){
      mask |= (1L << i);
      *pte ^= PTE_A;
    }
  }

  return copyout(p->pagetable, buf, (char *)&mask, sizeof(mask));
}
#endif

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
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

// set trace mask.
uint64
sys_trace(void)
{
  int mask;
  argint(0, &mask);
  struct proc *p = myproc();
  if(mask < 0)
    return -1;

  p->trace_mask = mask;
  return 0;
}

// return sysinfo.
uint64
sys_sysinfo(void)
{
  struct sysinfo info;
  info.freemem = freemem();
  info.nproc = nproc();

  uint64 addr;
  argaddr(0, &addr);

  return copyout(myproc()->pagetable, addr, (char *)&info, sizeof(info));
}

// set alarm.
uint64
sys_sigalarm(void)
{
  int ticks;
  uint64 handler;
  argint(0, &ticks);
  argaddr(1, &handler);
  if(ticks < 0)
    return -1;

  return sigalarm(ticks, (void (*)(void))handler);
}

// sigreturn.
uint64
sys_sigreturn(void)
{
  return sigreturn();
}
