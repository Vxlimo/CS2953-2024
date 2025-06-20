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

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

#ifdef LAB_COW
int ref_count[PHYSTOP / PGSIZE]; // reference count for the page
struct spinlock ref_count_lock;
#define PAGENUM(pa) ((uint64)(pa) / PGSIZE)
#endif

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
  char lockname[10];
} kmem[NCPU];

void
kinit()
{
  // For every possible CPU, set up a kmem structure.
  for(int id = 0; id < NCPU; id++)
  {
    kmem[id].freelist = 0;
    #ifdef LAB_LOCK
    snprintf(kmem[id].lockname, sizeof(kmem[id].lockname), "kmem%d", id);
    #endif
    initlock(&kmem[id].lock, kmem[id].lockname);
  }
  #ifdef LAB_COW
  memset(ref_count, 0, sizeof(ref_count));
  #endif
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  r = (struct run*)pa;

  #ifdef LAB_COW
  acquire(&ref_count_lock);
  ref_count[PAGENUM(r)]--;
  // Still referenced by some process.
  if(ref_count[PAGENUM(r)] > 0)
  {
    release(&ref_count_lock);
    return;
  }

  // No one is using this page.
  ref_count[PAGENUM(r)] = 0;
  release(&ref_count_lock);
  #endif

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  // Add the page to the free list of current CPU.
  // We need to turn off interrupts.
  push_off();
  int id = cpuid();
  acquire(&kmem[id].lock);
  r->next = kmem[id].freelist;
  kmem[id].freelist = r;
  release(&kmem[id].lock);
  pop_off();
}

// Find the middle node of the free list.
void *
find_mid(struct run *r)
{
  struct run *slow = r;
  struct run *fast = r;
  while(fast && fast->next)
  {
    slow = slow->next;
    fast = fast->next->next;
  }
  return (void*)slow;
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  // Try to allocate from the current CPU's free list.
  // We need to turn off interrupts.
  push_off();
  int id = cpuid();
  acquire(&kmem[id].lock);

  // If the current CPU's free list is empty, try to
  // allocate from the free list of another CPU.
  if(!kmem[id].freelist)
  {
    for(int new_id = 0; new_id < NCPU; new_id++)
    {
      if(new_id == id)
        continue;
      acquire(&kmem[new_id].lock);
      r = kmem[new_id].freelist;
      if(r)
      {
        struct run *mid = find_mid(r);
        // Move half of the free list to the current CPU.
        kmem[new_id].freelist = mid->next;
        mid->next = 0;
        // Add the page to the free list of current CPU.
        kmem[id].freelist = r;
        release(&kmem[new_id].lock);
        break;
      }
      release(&kmem[new_id].lock);
    }
  }

  r = kmem[id].freelist;
  if(r)
    kmem[id].freelist = r->next;
  release(&kmem[id].lock);
  pop_off();

  if(r)
  {
    memset((char*)r, 5, PGSIZE); // fill with junk
    #ifdef LAB_COW
    acquire(&ref_count_lock);
    ref_count[PAGENUM(r)] = 1; // reference count for the page
    release(&ref_count_lock);
    #endif
  }
  return (void*)r;
}

#ifdef LAB_SYSCALL
// Return the amount of free physical memory (in bytes).
uint64
freemem(void)
{
  uint64 total = 0;
  struct run *r;

  for(int id = 0; id < NCPU; id++)
  {
    acquire(&kmem[id].lock);
    // for each free page, add PGSIZE to total
    for(r = kmem[id].freelist; r; r = r->next)
      total += PGSIZE;
    release(&kmem[id].lock);
  }
  return total;
}
#endif

#ifdef LAB_COW
// Increment the reference count for the page pointed to by pa.
void
kalloc_cow(void *pa)
{
  acquire(&ref_count_lock);
  ref_count[PAGENUM(pa)]++;
  release(&ref_count_lock);
}
#endif
