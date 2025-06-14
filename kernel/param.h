#ifdef LAB_FS
#define NPROC        10  // maximum number of processes
#else
#define NPROC        64  // maximum number of processes (speedsup bigfile)
#endif
#define NCPU          8  // maximum number of CPUs
#define NOFILE       16  // open files per process
#define NFILE       100  // open files per system
#define NINODE       50  // maximum number of active i-nodes
#define NDEV         10  // maximum major device number
#define ROOTDEV       1  // device number of file system root disk
#define MAXARG       32  // max exec arguments
#define MAXOPBLOCKS  10  // max # of blocks any FS op writes
#define LOGSIZE      (MAXOPBLOCKS*3)  // max data blocks in on-disk log
#define NBUCKET      17  // number of hash buckets in buffer cache
#define NBUF         (NBUCKET*3)  // size of disk block cache
#ifdef LAB_FS
#define FSSIZE       200000  // size of file system in blocks
#else
#ifdef LAB_LOCK
#define FSSIZE       10000  // size of file system in blocks
#else
#define FSSIZE       2000   // size of file system in blocks
#endif
#endif
#define MAXPATH      128   // maximum file path name
#ifdef LAB_FS
#define MAX_LINK_DEPTH 40
#endif
#ifdef LAB_MMAP
#define NMMAPVMA     16   // maximum number of VMA per process
#endif
