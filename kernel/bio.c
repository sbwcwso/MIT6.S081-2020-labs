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

static struct buf free_buffers;


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

  free_buffers.next = &free_buffers;
  free_buffers.prev = &free_buffers;
  for(int i = 0; i < NBUF; i++) {
    struct buf *b = &bcache.buf[i];
    // add to free list
    b->next = free_buffers.next;
    b->prev = &free_buffers;
    free_buffers.next->prev = b;
    free_buffers.next = b;
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

  if (free_buffers.next == &free_buffers) {
    // reclaim buffers from buckets, at most one from each
    for (int i = 0; i < BUCKETS; i++) {
      struct bucket *bkt = &buckets[i];
      struct buf *free_buf = 0;
      acquire(&bkt->lock);
      for (struct buf *buf = bkt->head.prev; buf != &bkt->head; buf = buf->prev) {
        if (buf->refcnt == 0) {
          free_buf = buf;
          buf->prev->next = buf->next;
          buf->next->prev = buf->prev;
          break;
        }
      }
      release(&bkt->lock);
      if (free_buf) {
        // add to free list in order of ticks
        struct buf *p = free_buffers.next;
        while (p != &free_buffers && p->ticks < free_buf->ticks) {
          p = p->next;
        }
        free_buf->next = p;
        free_buf->prev = p->prev;
        p->prev->next = free_buf;
        p->prev = free_buf;
      }
    }
    if (free_buffers.next == &free_buffers) {
      release(&bcache.lock);
      panic("bget: no free buffers");
    }
  }

  struct buf *buf = free_buffers.next;
  // remove from free list
  free_buffers.next = buf->next;
  buf->next->prev = &free_buffers;

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
