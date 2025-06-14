#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#ifdef LAB_MMAP
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#endif

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

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

#ifdef LAB_MMAP
uint64
mmapfault(struct proc *p, struct vma* mmap)
{
  struct file *f = mmap->fd;
  if(f == 0)
    return -1;

  uint64 pas[mmap->len / PGSIZE];
  for(int i = 0; i < mmap->len; i += PGSIZE){
    uint64 pa = (uint64)kalloc();
    pas[i / PGSIZE] = pa;
    if(pa == 0)
      goto err;
    memset((void*)pa, 0, PGSIZE);

    ilock(f->ip);
    // Read the page from the file into the allocated memory.
    if(readi(f->ip, 0, pa, mmap->offset + i, PGSIZE) < 0){
      iunlock(f->ip);
      goto err; // read failed
    }
    iunlock(f->ip);
    // Map the page into the process's address space.
    if(mappages(p->pagetable, mmap->addr + i, PGSIZE, pa, (mmap->prot << 1) | PTE_V | PTE_U) < 0)
      goto err; // mapping failed
  }
  mmap->mapped = 1;
  return 0;

  err:
  for(int i = 0; i < mmap->len / PGSIZE; i++){
    if(pas[i] != 0)
      kfree((void*)pas[i]);
  }
  return -1;
}
#endif

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

    if(killed(p))
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sepc, scause, and sstatus,
    // so enable only now that we're done with those registers.
    intr_on();

    syscall();
  }
  #ifdef LAB_COW
  else if(r_scause() == 15){
    // page fault
    uint64 va = r_stval();
    if(va >= MAXVA || va >= p->sz)
      goto err;

    pte_t *pte = walk(p->pagetable, va, 0);
    if(pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0 || (*pte & PTE_COW) == 0)
      goto err;

    char *mem = kalloc();
    if(mem == 0)
      goto err;

    uint64 pa = PTE2PA(*pte);
    uint flags = PTE_FLAGS(*pte);
    memmove(mem, (char*)pa, PGSIZE);
    *pte = PA2PTE((uint64)mem) | flags | PTE_W;
    *pte &= ~PTE_COW;
    kfree((void*)pa);
  }
  #endif
  #ifdef LAB_MMAP
  else if(r_scause() == 13){
    // page fault of mmap
    uint64 va = r_stval();
    if(va >= MAXVA)
      goto err;

    // check if the address is in a valid mmap region.
    int found = 0;
    for(int i = 0; i < NMMAPVMA; i++){
      if(p->mmap[i].valid && p->mmap[i].addr <= va && va < p->mmap[i].addr + p->mmap[i].len){
        found = 1;
        if(mmapfault(p, &p->mmap[i]) == -1)
          goto err;
        break;
      }
    }
    if(!found)
      goto err;
  }
  #endif
  else if((which_dev = devintr()) != 0){
    // ok
  } else {
    #ifdef LAB_COW
    err:
    #endif
    #ifdef LAB_MMAP
    err:
    #endif
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    setkilled(p);
  }

  if(killed(p))
    exit(-1);

  // a timer interrupt.
  if(which_dev == 2)
  {
    #ifdef LAB_TRAPS
    // check if the process has an alarm.
    if(p->alarm_period > 0 && !p->alarm_handling)
    {
      if(++p->alarm_cur_tick == p->alarm_period)
      {
        p->alarm_cur_tick = 0;
        // restore user trap frame,
        // then set the program counter to the alarm handler.
        *p->user_trap_frame = *p->trapframe;
        p->trapframe->epc = p->alarm_handler;
        p->alarm_handling = 1;
      }
    }
    #endif
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

  // send syscalls, interrupts, and exceptions to uservec in trampoline.S
  uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
  w_stvec(trampoline_uservec);

  // set up trapframe values that uservec will need when
  // the process next traps into the kernel.
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

  // jump to userret in trampoline.S at the top of memory, which
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
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
