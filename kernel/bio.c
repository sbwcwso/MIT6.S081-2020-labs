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

#define BUCKETS 13

struct bucket {
  struct spinlock lock;
  struct buf head;
  char lock_name[20];
} buckets[BUCKETS];

struct {
  struct spinlock lock;
  struct buf buf[NBUF];
} bcache;

static struct buf lru_free_buffers;

static inline void insert_to_lru_free_buffers(struct buf *b) {
  // insert b to lru_free_buffers in order of ticks
  struct buf *p = lru_free_buffers.fnext;
  while (p != &lru_free_buffers && p->oldticks < b->oldticks) {
    p = p->fnext;
  }
  b->fnext = p;
  b->fprev = p->fprev;
  p->fprev->fnext = b;
  p->fprev = b;
}

void
binit(void)
{

  initlock(&bcache.lock, "bcache");
  for (int i = 0; i < BUCKETS; i++) {
    snprintf(buckets[i].lock_name, sizeof(buckets[i].lock_name), "bcache.bucket.%d", i);
    initlock(&buckets[i].lock, buckets[i].lock_name);
    buckets[i].head.next = &buckets[i].head;
    buckets[i].head.prev = &buckets[i].head;
  }

  lru_free_buffers.fnext = &lru_free_buffers;
  lru_free_buffers.fprev = &lru_free_buffers;
  for(int i = 0; i < NBUF; i++) {
    struct buf *b = &bcache.buf[i];
    // add to free list
    b->fnext = lru_free_buffers.fnext;
    b->fprev = &lru_free_buffers;
    lru_free_buffers.fnext->fprev = b;
    lru_free_buffers.fnext = b;
    initsleeplock(&bcache.buf[i].lock, "buffer");
  }
}

static inline struct buf* find_in_bucket(struct bucket *bucket, uint dev, uint blockno) {
  acquire(&bucket->lock);
  for (struct buf *b = bucket->head.next; b != &bucket->head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bucket->lock);
      return b;
    }
  }
  release(&bucket->lock);
  return 0;
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{

  int bucket_idx = (dev + blockno) % BUCKETS;
  struct bucket *bucket = &buckets[bucket_idx];
  struct buf *b = find_in_bucket(bucket, dev, blockno);
  if (b != 0) {
    acquiresleep(&b->lock);
    return b;
  }
  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.

  acquire(&bcache.lock);
  // find again, in case another cpu added it
  b = find_in_bucket(bucket, dev, blockno);
  if (b != 0) {
    release(&bcache.lock);
    acquiresleep(&b->lock);
    return b;
  }

  if (lru_free_buffers.fnext == &lru_free_buffers) {
    // reclaim buffers from buckets, at most one from each
empty:
    for (int i = 0; i < NBUF; i++) {
      struct buf *buf = &bcache.buf[i];
      if (buf->refcnt == 0) {
        buf->oldticks = buf->ticks;
        insert_to_lru_free_buffers(buf);
      }
    }
  }

  struct buf *buf;
non_empty:
  buf = lru_free_buffers.fnext;
  if (buf == &lru_free_buffers) 
    goto empty;
  // remove from free list
  lru_free_buffers.fnext = buf->fnext;
  buf->fnext->fprev = &lru_free_buffers;
  if (buf->valid) {
    int old_bucket_idx = (buf->dev + buf->blockno) % BUCKETS;
    struct bucket *old_bucket = &buckets[old_bucket_idx];
    acquire(&old_bucket->lock);
    if (buf->refcnt != 0 || buf->ticks != buf->oldticks) {
      // someone hold it again
      // or it was used then free again, so the ticks changed
      release(&old_bucket->lock);
      goto non_empty;
    }
    // remove from old bucket
    buf->prev->next = buf->next;
    buf->next->prev = buf->prev;
    release(&old_bucket->lock);
  } 

  buf->refcnt = 1;
  buf->dev = dev;
  buf->blockno = blockno;
  buf->valid = 0;

  acquire(&bucket->lock);
  buf->next = bucket->head.next;
  buf->prev = &bucket->head;
  bucket->head.next->prev = buf;
  bucket->head.next = buf;
  release(&bucket->lock);

  release(&bcache.lock);

  acquiresleep(&buf->lock);
  return buf;
  
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

  int bucket_idx = (b->dev + b->blockno) % BUCKETS;
  struct bucket *bucket = &buckets[bucket_idx];
  acquire(&bucket->lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->ticks = ticks;  // Update ticks on release
    // move to the head of the bucket's list
    b->prev->next = b->next;
    b->next->prev = b->prev;
    b->next = bucket->head.next;
    b->prev = &bucket->head;
    bucket->head.next->prev = b;
    bucket->head.next = b;
  }
  release(&bucket->lock);

}

void
bpin(struct buf *b) {
  int bucket_idx = (b->dev + b->blockno) % BUCKETS;
  struct bucket *bucket = &buckets[bucket_idx];
  acquire(&bucket->lock);
  b->refcnt++;
  release(&bucket->lock);
}

void
bunpin(struct buf *b) {
  int bucket_idx = (b->dev + b->blockno) % BUCKETS;
  struct bucket *bucket = &buckets[bucket_idx];
  acquire(&bucket->lock);
  b->refcnt--;
  release(&bucket->lock);
}
