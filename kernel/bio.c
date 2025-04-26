// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

struct {
  struct spinlock lock;
  struct buf buf[NBUF];
} bcache;
struct {
  struct spinlock lock;

  // linked list of buffers in this bucket
  struct buf head;
} bcache_buckets[NBUCKET];

void
binit(void)
{
  struct buf *b = bcache.buf;

  initlock(&bcache.lock, "bcache");
  for(int i = 0; i < NBUCKET; i++) {
    initlock(&bcache_buckets[i].lock, "bcache bucket");
    bcache_buckets[i].head.bnext = &bcache_buckets[i].head;
    bcache_buckets[i].head.bprev = &bcache_buckets[i].head;
  }

  for(int i = 0; i < NBUF; i++, b++) {
    initsleeplock(&bcache.buf[i].lock, "bcache buf");
    int bucket = i % NBUCKET;
    b->blockno = i;
    b->bnext = bcache_buckets[bucket].head.bnext;
    b->bprev = &bcache_buckets[bucket].head;
    bcache_buckets[bucket].head.bnext = b;
    b->bnext->bprev = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  int bucket = blockno % NBUCKET;
  acquire(&bcache_buckets[bucket].lock);

  // Is the block already cached?
  for(b = bcache_buckets[bucket].head.bnext; b != &bcache_buckets[bucket].head; b = b->bnext){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache_buckets[bucket].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Try to find an unused buffer in the bucket.
  for(b = bcache_buckets[bucket].head.bnext; b != &bcache_buckets[bucket].head; b = b->bnext){
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache_buckets[bucket].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Recycle from other buckets.
  acquire(&bcache.lock);
  for(int i = 0; i < NBUCKET; i++) {
    if(i == bucket)
      continue;
    acquire(&bcache_buckets[i].lock);
    for(b = bcache_buckets[i].head.bnext; b != &bcache_buckets[i].head; b = b->bnext){
      if(b->refcnt == 0) {
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;
        break;
      }
    }
    // If we found a buffer to recycle, move it to the head of the
    // list in the bucket we are using.
    if(b != &bcache_buckets[i].head) {
      b->bnext->bprev = b->bprev;
      b->bprev->bnext = b->bnext;
      b->bnext = bcache_buckets[bucket].head.bnext;
      b->bprev = &bcache_buckets[bucket].head;
      bcache_buckets[bucket].head.bnext->bprev = b;
      bcache_buckets[bucket].head.bnext = b;
      release(&bcache_buckets[i].lock);
      release(&bcache_buckets[bucket].lock);
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
    else
      release(&bcache_buckets[i].lock);
  }
  release(&bcache.lock);

  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  int bucket = b->blockno % NBUCKET;
  acquire(&bcache_buckets[bucket].lock);
  b->refcnt--;

  if (b->refcnt == 0) {
    // no one is waiting for it.
    ; // do nothing
  }

  releasesleep(&b->lock);
  release(&bcache_buckets[bucket].lock);
}

void
bpin(struct buf *b) {
  int bucket = b->blockno % NBUCKET;
  acquire(&bcache_buckets[bucket].lock);
  b->refcnt++;
  release(&bcache_buckets[bucket].lock);
}

void
bunpin(struct buf *b) {
  int bucket = b->blockno % NBUCKET;
  acquire(&bcache_buckets[bucket].lock);
  b->refcnt--;
  release(&bcache_buckets[bucket].lock);
}
