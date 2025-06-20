//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#ifdef LAB_MMAP
#include "memlayout.h"
#endif

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  argaddr(1, &st);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0){
    iunlockput(dp);
    return 0;
  }

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      goto fail;
  }

  if(dirlink(dp, name, ip->inum) < 0)
    goto fail;

  if(type == T_DIR){
    // now that success is guaranteed:
    dp->nlink++;  // for ".."
    iupdate(dp);
  }

  iunlockput(dp);

  return ip;

 fail:
  // something went wrong. de-allocate ip.
  ip->nlink = 0;
  iupdate(ip);
  iunlockput(ip);
  iunlockput(dp);
  return 0;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  argint(1, &omode);
  if((n = argstr(0, path, MAXPATH)) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  #ifdef LAB_FS
  if(ip->type == T_SYMLINK && !(omode & O_NOFOLLOW)){
    int depth = 0;
    char target[MAXPATH];
    while(ip->type == T_SYMLINK){
      if(depth++ >= MAX_LINK_DEPTH){
        iunlockput(ip);
        end_op();
        return -1;
      }
      if(readi(ip, 0, (uint64)target, 0, MAXPATH) < 0){
        iunlockput(ip);
        end_op();
        return -1;
      }
      iunlockput(ip);
      if((ip = namei(target)) == 0){
        end_op();
        return -1;
      }
      ilock(ip);
    }
  }
  #endif

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  argint(1, &major);
  argint(2, &minor);
  if((argstr(0, path, MAXPATH)) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  argaddr(1, &uargv);
  if(argstr(0, path, MAXPATH) < 0) {
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = exec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  argaddr(0, &fdarray);
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}

#ifdef LAB_FS
uint64
sys_symlink(void)
{
  char target[MAXPATH], path[MAXPATH];
  if(argstr(0, target, MAXPATH) < 0 || argstr(1, path, MAXPATH) < 0) {
    return -1;
  }

  begin_op();
  struct inode *ip = create(path, T_SYMLINK, 0, 0);
  if(ip == 0) {
    end_op();
    return -1;
  }

  if(writei(ip, 0, (uint64)target, 0, strlen(target)) != strlen(target)) {
    iunlockput(ip);
    end_op();
    return -1;
  }

  iunlockput(ip);
  end_op();
  return 0;
}
#endif

#ifdef LAB_MMAP
// Memory mapping system calls.
// Assume length is PGSIZE aligned.
uint64
sys_mmap(void)
{
  uint64 addr;
  int length, prot, flags, fd, offset;
  struct file *f;

  argaddr(0, &addr);
  argint(1, &length);
  argint(2, &prot);
  argint(3, &flags);
  argfd(4, &fd, &f);
  argint(5, &offset);

  // Invalid arguments
  if(addr >= MAXVA || length <= 0 || (addr % PGSIZE != 0))
    return -1;
  // File must be valid and readable
  if(fd < 0 || fd >= NOFILE || f == 0 || f->readable == 0)
    return -1;
  // Check permissions
  if((prot & PROT_WRITE) && f->writable == O_RDONLY && (flags & MAP_SHARED))
    return -1;

  struct proc *p = myproc();

  // Here always let kernel choose the address
  if(addr == 0) {
    // Find an available mmap slot
    int mmap_id = -1;
    for(int i = 0; i < NMMAPVMA; i++) {
      if(p->mmap[i].addr == 0) {
        mmap_id = i;
        break;
      }
    }
    if(mmap_id == -1) {
      return -1;
    }

    // Find an available address
    int occupied[NMMAPVMA] = {-1};
    for(int i = 0; i < NMMAPVMA; i++) {
      if(p->mmap[i].valid)
        occupied[i] = i;
    }
    for(int i = 0; i < NMMAPVMA; i++)
    {
      for(int j = i; j < NMMAPVMA; j++)
      {
        if(occupied[i] == -1 && occupied[j] == -1)
          continue;
        if(i == -1 || p->mmap[occupied[i]].addr < p->mmap[occupied[j]].addr)
        {
          int temp = occupied[i];
          occupied[i] = occupied[j];
          occupied[j] = temp;
        }
      }
    }
    uint64 addr = MAXMMAP - length;
    for(int i = 0; i < NMMAPVMA; i++) {
      if(occupied[i] == -1)
        break;
      if(addr >= p->mmap[occupied[i]].addr + p->mmap[occupied[i]].len)
        break;
      if(addr + length > p->mmap[occupied[i]].addr) {
        addr = p->mmap[occupied[i]].addr - length;
      }
    }

    p->mmap[mmap_id].valid = 1;
    p->mmap[mmap_id].addr = addr;
    p->mmap[mmap_id].len = length;
    p->mmap[mmap_id].prot = prot;
    p->mmap[mmap_id].flags = flags;
    p->mmap[mmap_id].fd = f;
    p->mmap[mmap_id].offset = offset;
    filedup(f); // Increment file reference count
    return addr;
  }

  return -1;
}

// Unmap a memory region.
uint64 unmap(struct proc *p, uint64 addr, int length)
{
  for(int i = 0; i < NMMAPVMA; i++) {
    if(p->mmap[i].valid && addr >= p->mmap[i].addr && addr < p->mmap[i].addr + p->mmap[i].len) {
      // If it's shared, we need to write back changes to the file.
      if(p->mmap[i].flags & MAP_SHARED) {
        for(int j = 0; j < length / PGSIZE; j++) {
          uint64 page_addr = addr + j * PGSIZE;
          // Check if the page is mapped
          if(walkaddr(p->pagetable, page_addr) == 0)
            continue;
          pte_t *pte = walk(p->pagetable, page_addr, 0);
          if(pte && (*pte & PTE_D)) {
            ilock(p->mmap[i].fd->ip);
            if(*pte & PTE_B){
              // The page is in bcache.
              uint64 addr = bmap(p->mmap[i].fd->ip, (p->mmap[i].offset + PGROUNDDOWN(page_addr - p->mmap[i].addr)) / BSIZE);
              struct buf *bp = bget(p->mmap[i].fd->ip->dev, addr);
              if(bp == 0) {
                iunlock(p->mmap[i].fd->ip);
                end_op();
                return -1; // read failed
              }
              bwrite(bp);
              brelse(bp);
            } else {
              begin_op();
              if(writei(p->mmap[i].fd->ip, 1, page_addr, p->mmap[i].offset + (page_addr - p->mmap[i].addr), PGSIZE) < 0) {
                iunlock(p->mmap[i].fd->ip);
                end_op();
                return -1;
              }
              end_op();
            }
            iunlock(p->mmap[i].fd->ip);
          }
        }
      }

      // Unmap the pages in the process's page table.
      for(int j = 0; j < length / PGSIZE; j++) {
        uint64 page_addr = addr + j * PGSIZE;
        if(walkaddr(p->pagetable, page_addr) == 0)
          continue;
        pte_t *pte = walk(p->pagetable, page_addr, 0);
        ilock(p->mmap[i].fd->ip);
        if(*pte & PTE_B) {
          // If the page is a block device, we need to unpin it.
          uint64 addr = bmap(p->mmap[i].fd->ip, (p->mmap[i].offset + PGROUNDDOWN(page_addr - p->mmap[i].addr)) / BSIZE);
          struct buf *bp = bget(p->mmap[i].fd->ip->dev, addr);
          if(bp == 0) {
            iunlock(p->mmap[i].fd->ip);
            return -1; // read failed
          }
          brelse(bp);
          uvmunmap(p->pagetable, page_addr, 1, 0);
          bunpin(bp);
        }
        else
          uvmunmap(p->pagetable, page_addr, 1, 1);
        iunlock(p->mmap[i].fd->ip);
      }

      // Update the mmap entry.
      if(addr == p->mmap[i].addr) {
        p->mmap[i].addr += length;
        p->mmap[i].len -= length;
        p->mmap[i].offset += length;
      }
      else if (addr + length == p->mmap[i].addr + p->mmap[i].len)
        p->mmap[i].len -= length;
      if(p->mmap[i].len == 0) {
        fileclose(p->mmap[i].fd); // Decrement file reference count
        p->mmap[i].valid = 0;
        p->mmap[i].addr = 0;
        p->mmap[i].len = 0;
        p->mmap[i].prot = 0;
        p->mmap[i].flags = 0;
        p->mmap[i].fd = 0;
        p->mmap[i].offset = 0;
      }
      return 0;
    }
  }

  return -1;
}

// Unmap a memory region.
// Assume length is PGSIZE aligned.
uint64
sys_munmap(void)
{
  uint64 addr;
  int length;

  argaddr(0, &addr);
  argint(1, &length);

  if(addr >= MAXVA || length <= 0 || (addr % PGSIZE != 0) || (length % PGSIZE != 0))
    return -1;

  struct proc *p = myproc();

  return unmap(p, addr, length);
}
#endif
