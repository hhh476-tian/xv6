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

#define NBUCKET 13

struct {
  struct spinlock lock;
  struct buf buf[NBUF];
  struct buf *buckets[NBUCKET];
  struct spinlock bucklocks[NBUCKET];
} bcache;

void buckadd(struct buf *b);
void buckdel(struct buf *b);
int inbuck(uint dev, uint blockno);
uint readticks();
void checker();

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");

  for(int i = 0; i < NBUCKET; i++) {
    initlock(&bcache.bucklocks[i], "bcache.bucket");
  }

  // put all bufs in bucket 0 
  bcache.buckets[0] = bcache.buf;
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->blockno = 0;
    b->next = b + 1;
    initsleeplock(&b->lock, "buffer");
  }
  (b-1)->next = 0; // correct the last one
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b, *lru;
  int buckno;
  int bi;
  uint oldest;

  buckno = blockno % NBUCKET;

  // check if in in cache
  acquire(&bcache.bucklocks[buckno]);
  bi = buckno % NBUCKET;
  b = bcache.buckets[bi];
  while (b) {
    if (b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&bcache.bucklocks[buckno]);
      acquiresleep(&b->lock);
      return b;
    }
    b = b->next;
  }

  // evict a buffer to make space
  lru = 0;
  oldest = ~0x0U; 
  for (int i = 0; i < NBUCKET; i++) {
    bi = (buckno + i) % NBUCKET;
    if (bi != buckno) {
      acquire(&bcache.bucklocks[bi]);
    }

    b = bcache.buckets[bi];
    while (b) {
      if (b->refcnt == 0 && b->lastuse < oldest) {
        oldest = b->lastuse;
        lru = b;
        break;
      }
      b = b->next;
    }

    if (bi != buckno) {
      release(&bcache.bucklocks[bi]);
    }
  }

  if (!lru) {
    release(&bcache.bucklocks[buckno]);
    panic("bget: no buffers");
  }

  buckdel(lru);

  lru->dev     = dev;
  lru->blockno = blockno;
  lru->valid   = 0;
  lru->refcnt  = 1;
  lru->lastuse = readticks();
  buckadd(lru);

  release(&bcache.bucklocks[buckno]);
  acquiresleep(&lru->lock);
  return lru;
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

  acquire(&bcache.bucklocks[b->blockno%NBUCKET]);
  b->refcnt--;
  release(&bcache.bucklocks[b->blockno%NBUCKET]);
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

// add a buf to its bucket, 
// must use with bucket locks held by caller
void
buckadd(struct buf *b) 
{
  int buckno = b->blockno % NBUCKET;
  struct buf *s = bcache.buckets[buckno];
  struct buf *prev;

  // is bucket empty?
  if (!s) {
    bcache.buckets[buckno] = b;
    b->next = 0;

    return;
  } 

  while (s) {
    if (b->dev == s->dev && b->blockno == s->blockno) {
      panic("Adding existing blockno to bucket");
    }
    prev = s;
    s = s->next;
  }

  prev->next = b;
  b->next = 0;
}

// remove a buf from its bucket, 
// must use with bucket locks held by caller
void
buckdel(struct buf *b)
{
  int buckno = b->blockno % NBUCKET;
  struct buf *s = bcache.buckets[buckno];
  struct buf *prev = 0;

  if (!s) {
    panic("bcache: empty bucket");
  }

  if(b->dev == s->dev && b->blockno == s->blockno) {
    bcache.buckets[buckno] = s->next;
    return;
  }

  while (s) {
    if(b->dev == s->dev && b->blockno == s->blockno) {
      prev->next = s->next;
      return;
    }
    prev = s;
    s = s->next;
  }

  panic("bcache delete");
}

// check if a buffer with given blockno in a given bucket
// caller need to hold the lock
int
inbuck(uint dev, uint blockno)
{
  int buckno = blockno % NBUCKET;
  struct buf *s = bcache.buckets[buckno];

  while (s) {
    if(dev == s->dev && blockno == s->blockno) {
      return 1;
    }
    s = s->next;
  }

  return 0;
}

uint
readticks()
{
  return ticks;
}

// check the invariance that there are NBUF buf in table
void
checker() 
{
  struct buf *b;
  int count=0;

  for(int i = 0; i < NBUCKET; i++){
    b = bcache.buckets[i];
    while (b) {
      count += 1;
      b = b->next; 
    }
  }

  if (count < NBUF - 3) {
    printf("missing buf, buf in table: %d\n", count);
    count = 0;
    for(int i = 0; i < NBUCKET; i++){
      b = bcache.buckets[i];
      printf("looking bucket: %d\n", i);
      while (b) {
        count += 1;
        printf("count: %d, bucket has block %d with ref counts: %d\n", count, b->blockno, b->refcnt);
        b = b->next; 
    }
  }
    panic("bache checker");
  }
}
