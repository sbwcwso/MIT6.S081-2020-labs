struct buf {
  int valid;   // has data been read from disk?
  int disk;    // does disk "own" buf?
  uint dev;
  uint blockno;
  uint ticks;  // For LRU tracking
  uint oldticks; // For LRU tracking
  struct sleeplock lock;
  uint refcnt;
  struct buf *prev; // bucket list
  struct buf *next;
  struct buf *fnext; // free list
  struct buf *fprev;
  uchar data[BSIZE];
};

