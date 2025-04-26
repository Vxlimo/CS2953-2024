struct buf {
  int valid;   // has data been read from disk?
  int disk;    // does disk "own" buf?
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  struct buf *bnext; // disk queue
  struct buf *bprev;
  uchar data[BSIZE];
};
