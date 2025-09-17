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

#define NBUCKET 71

struct {
  struct spinlock lock;
  struct buf buf[NBUF];
  struct buf *buckets[NBUCKET];
  struct spinlock bucklocks[NBUCKET];
} bcache;

int bhash(uint blockno);

extern uint ticks;

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");

  for (int i = 0; i < NBUF; i++) {
    b = &bcache.buf[i];
    initsleeplock(&b->lock, "buffer");
  }

  for(int i = 0; i < NBUCKET; i++){
    // b = &bcache.buckets[i];
    if (i >= NBUF) {
      bcache.buckets[i] = &bcache.buf[0];
    } else {
      bcache.buckets[i] = &bcache.buf[i];
    }
    initlock(&bcache.bucklocks[i], "bcache.bucket");
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  int index;
  int lru;
  uint maxticks;
  acquire(&bcache.lock);

  // Is the block already cached?
  index = bhash(blockno);
  acquire(&bcache.bucklocks[index]);
  b = bcache.buckets[index];
  if(b->dev == dev && b->blockno == blockno){
    b->refcnt++;
    release(&bcache.bucklocks[index]);
    release(&bcache.lock);
    acquiresleep(&b->lock);
    return b;
  }
  release(&bcache.bucklocks[index]);

  // linear search an empty slot in hash table
  for (int i = index; i < NBUCKET; i++) {
    acquire(&bcache.bucklocks[i]);
    b = bcache.buckets[i];
    if (b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&bcache.bucklocks[i]);
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
    release(&bcache.bucklocks[i]);
  }

  for (int i = 0; i < index; i++) {
    acquire(&bcache.bucklocks[i]);
    b = bcache.buckets[i];
    if (b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&bcache.bucklocks[i]);
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
    release(&bcache.bucklocks[i]);
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  // acquire(&bcache.lock);
  maxticks = 0;
  lru = -1;
  for (int i = index; i < NBUCKET; i++) {
    acquire(&bcache.bucklocks[i]);
    b = bcache.buckets[i];
    if(b->refcnt == 0) {
        if (b->lastuse >= maxticks) {
          maxticks = b->lastuse;
          lru = i;
        }
    }
    release(&bcache.bucklocks[i]);
  }

  for (int i = 0; i < index; i++) {
    acquire(&bcache.bucklocks[i]);
    b = bcache.buckets[i];
    if(b->refcnt == 0) {
        if (b->lastuse >= maxticks) {
          maxticks = b->lastuse;
          lru = i;
        }
    }
    release(&bcache.bucklocks[i]);
  } 

  if (lru != -1) {
    b = bcache.buckets[lru];
    b->dev = dev;
    b->blockno = blockno;
    b->valid = 0;
    b->lastuse = ticks;
    b->refcnt = 1;
    release(&bcache.lock);
    acquiresleep(&b->lock);
    return b;
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

  releasesleep(&b->lock);

  for(int i = 0; i < NBUCKET; i++){
    acquire(&bcache.bucklocks[i]);
    if(b->blockno == bcache.buckets[i]->blockno){
      bcache.buckets[i]->refcnt--;
      release(&bcache.bucklocks[i]);
      break;
    }
    release(&bcache.bucklocks[i]);
  }
}

void
bpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt++;
  release(&bcache.lock);
}

void
bunpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt--;
  release(&bcache.lock);
}

// simple hash function by blockno
int 
bhash(uint blockno) {
  return (int)blockno % NBUCKET;
}

