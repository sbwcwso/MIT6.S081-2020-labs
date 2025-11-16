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
#include "memlayout.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0)
    return -1;
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

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
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

  if(argfd(0, 0, &f) < 0 || argaddr(1, &st) < 0)
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

  if((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if(dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);

  return ip;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  if((n = argstr(0, path, MAXPATH)) < 0 || argint(1, &omode) < 0)
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
  if((argstr(0, path, MAXPATH)) < 0 ||
     argint(1, &major) < 0 ||
     argint(2, &minor) < 0 ||
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

  if(argstr(0, path, MAXPATH) < 0 || argaddr(1, &uargv) < 0){
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

  if(argaddr(0, &fdarray) < 0)
    return -1;
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

// allocate a vma page for mmaped file
int
alloc_mmap_page(struct proc *p, uint64 va) 
{
  if ( va < MMAP_START || va >= TRAPFRAME)
    return -1;
  for (int i = 0; i < MAX_MMAP_AREAS; i++) {
    if (p->mmap_areas[i].in_use == 1) {
      struct mmap_area *area = &p->mmap_areas[i];
      if (area->addr <= va && va < area->addr + area->length) {
        uint64 pa = (uint64)kalloc();
        if (pa == 0) {
          return -1;
        }

        int perm = PTE_U | PTE_V;
        va = PGROUNDDOWN(va);
        if (area->prot & PROT_READ) perm |= PTE_R;
        if (area->prot & PROT_WRITE) perm |= PTE_W;
        if (mappages(p->pagetable, va, PGSIZE, pa, perm) < 0) {
          kfree((void*)pa);
          return -1;
        }

        memset((void*)pa, 0, PGSIZE);

        ilock(area->file->ip);
        readi(area->file->ip, 0, pa, va - area->addr + area->offset, PGSIZE);
        iunlock(area->file->ip);
        return 0;
      }
    }
  }

  return -1; // the va not in mmap area
}

uint64 
sys_mmap(void) 
{

  uint64 addr;
  uint64 length;
  int prot;
  int flags;
  int fd;
  int offset;

  argaddr(0, &addr);
  argaddr(1, &length);
  argint(2, &prot);
  argint(3, &flags);
  argint(4, &fd);
  argint(5, &offset);

  if(addr != 0 || length == 0 || offset != 0)
    return -1;

  length = PGROUNDUP(length);
  struct proc *p = myproc();
  int i;
  for (i = 0; i < MAX_MMAP_AREAS; i++) {
    if (p->mmap_areas[i].in_use == 0) {
      p->mmap_areas[i].addr = MMAP_ADDR(i);
      p->mmap_areas[i].length = length;
      p->mmap_areas[i].prot = prot;
      p->mmap_areas[i].flags = flags;
      struct file *f;
      if (fd >= NOFILE || (f = p->ofile[fd]) == 0) {
        return -1;
      }

      // should be able to map file opened read-only with private writable
      // check that mmap doesn't allow read/write mapping of a
      // file opened read-only.
      if ((prot & PROT_READ) && !f->readable)
        return -1;
      if ((prot & PROT_WRITE) && !f->writable && (flags & MAP_SHARED))
        return -1;

      p->mmap_areas[i].file = filedup(f); // increase file reference count

      p->mmap_areas[i].offset = offset;
      p->mmap_areas[i].in_use = 1;

      return p->mmap_areas[i].addr;
    }
  }
  release(&p->lock);
  return -1;

  // return -1;
}

// Helper function to write back a page to file if needed
// Unified helper: optionally write back len bytes starting at va within page
// and optionally unmap the full page containing va. Avoids duplicate walkaddr
// and lock/unlock patterns present in the previous separate helpers.
static void
munmap_apply(pagetable_t pagetable, struct mmap_area *area, uint64 va,
             int len, int do_writeback, int do_unmap)
{
  // len may be 0 (when only freeing) or <= PGSIZE for partial boundary writes.
  uint64 pa = walkaddr(pagetable, va);
  if(pa == 0)
    return; // page not present; silently ignore (lazy allocation case)

  if(do_writeback && len > 0){
    ilock(area->file->ip);
    writei(area->file->ip, 0, pa, va - area->addr + area->offset, len);
    iunlock(area->file->ip);
  }
  if(do_unmap){
    // uvmunmap expects page-aligned virtual address
    uint64 pageva = PGROUNDDOWN(va);
    uvmunmap(pagetable, pageva, 1, 1);
  }
}

// Helper to handle partial pages at boundaries
static void
munmap_handle_partial_pages(pagetable_t pagetable, struct mmap_area *area,
                            uint64 unmap_start, uint64 unmap_end,
                            uint64 first_page, uint64 last_page,
                            int needs_writeback, int final_free)
{
  int single_page = (first_page > last_page);
  if(single_page){
    int bytes = unmap_end - unmap_start;
    munmap_apply(pagetable, area, unmap_start, bytes,
                 needs_writeback, final_free);
    return;
  }
  // Start partial
  uint64 start_bytes = first_page - unmap_start;
  if(start_bytes > 0)
    munmap_apply(pagetable, area, unmap_start, start_bytes,
                 needs_writeback, final_free);
  // End partial
  uint64 end_bytes = unmap_end - last_page;
  if(end_bytes > 0)
    munmap_apply(pagetable, area, last_page, end_bytes,
                 needs_writeback, final_free);
}

uint64 
munmap_helper(uint64 addr, uint64 length) 
{
  struct proc *p = myproc();
  
  // Find the mmap area containing this address
  for (int i = 0; i < MAX_MMAP_AREAS; i++) {
    struct mmap_area *area = &p->mmap_areas[i];
    if (!area->in_use || addr < area->addr || addr > area->addr + area->length) {
      continue;  // Not the right area
    }

    // Calculate actual unmap range (clamp to area boundaries)
    uint64 unmap_start = addr;
    uint64 unmap_end = addr + length;
    if (unmap_end < length) {  // Overflow check
      return -1;
    }
    uint64 area_end = area->addr + area->length;
    if (unmap_end > area_end) {
      unmap_end = area_end;
    }

    // Determine page-aligned boundaries
    uint64 first_aligned_page = PGROUNDUP(unmap_start);
    uint64 last_aligned_page = PGROUNDDOWN(unmap_end);
    
    // Check if we need to write back to file (MAP_SHARED + writable)
    int needs_writeback = (area->prot & PROT_WRITE) && (area->flags & MAP_SHARED);

    // Step 1: Free all fully-aligned pages and write them back if needed
    begin_op();
    for(uint64 va = first_aligned_page; va < last_aligned_page; va += PGSIZE)
      munmap_apply(p->pagetable, area, va, PGSIZE, needs_writeback, 1);

    // Step 2: Handle boundary partial pages (writeback only now; free later if whole region)
    munmap_handle_partial_pages(p->pagetable, area, unmap_start, unmap_end,
                                first_aligned_page, last_aligned_page,
                                needs_writeback, 0);
    end_op();

    // Step 3: Update or remove the area based on what was unmapped
    int unmapping_whole_region = (addr == area->addr && length >= area->length);
    int unmapping_from_start = (addr == area->addr);
    int unmapping_from_end = (unmap_end >= area_end);

    if (unmapping_whole_region) {
      // Free partial pages (without writeback, already done above)
      munmap_handle_partial_pages(p->pagetable, area, unmap_start, unmap_end,
                                  first_aligned_page, last_aligned_page, 0, 1);
      fileclose(area->file);
      area->in_use = 0;
      return 0;
    } else if (unmapping_from_start) {
      // Shrink from the start
      if(first_aligned_page <= last_aligned_page && (first_aligned_page - unmap_start) > 0)
        munmap_apply(p->pagetable, area, unmap_start, 0, 0, 1);
      area->addr += length;
      area->length -= length;
      return 0;
    } else if (unmapping_from_end) {
      // Shrink from the end
      if(first_aligned_page <= last_aligned_page && (unmap_end - last_aligned_page) > 0)
        munmap_apply(p->pagetable, area, last_aligned_page, 0, 0, 1);
      area->length -= length;
      return 0;
    } else {
      // Punching hole in middle - not supported
      panic("munmap: cannot punch hole in middle of region");
    }
  }

  return -1;  // Address not found in any mmap area
}

// An munmap call might cover only a portion of an mmap-ed region, but you can assume that it will either unmap at the start, or at the end, or the whole region (but not punch a hole in the middle of a region).
uint64 sys_munmap(void) {
  uint64 addr;
  uint64 length;

  argaddr(0, &addr);
  argaddr(1, &length);

  if (length == 0)
    return -1;

  return munmap_helper(addr, length);
} 
