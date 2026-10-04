#include "types.h"
#include "param.h"
#include "riscv.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "stat.h"
#include "sleeplock.h"
#include "file.h"

// A process has one executing thread. Only it changes its VMAs; p->lock
// protects publication/unmapping against psinfo's remote page-table walk.
// Disk I/O and fileclose must never run under this spinlock.
static struct vma *
lookup(struct proc *p, uint64 va)
{
  for (int i = 0; i < NVMA; i++)
    if (p->vmas[i].start <= va && va < p->vmas[i].end)
      return &p->vmas[i];
  return 0;
}

uint64
sys_mmap(void)
{
  uint64 hint, len, off, start;
  int prot, flags, fd, slot = -1;
  struct proc *p = myproc();
  struct file *f = 0;
  argaddr(0, &hint);
  argaddr(1, &len);
  argint(2, &prot);
  argint(3, &flags);
  argint(4, &fd);
  argaddr(5, &off);
  if (hint != 0 || len == 0 || len > MMAPMAX ||
      (prot != PROT_READ && prot != (PROT_READ | PROT_WRITE)) ||
      (flags != MAP_PRIVATE && flags != (MAP_PRIVATE | MAP_ANONYMOUS)) ||
      off % PGSIZE)
    return -1;
  len = PGROUNDUP(len);
  if (flags & MAP_ANONYMOUS) {
    if (fd != -1 || off != 0)
      return -1;
  } else {
    if (fd < 0 || fd >= NOFILE || (f = p->ofile[fd]) == 0 ||
        f->type != FD_INODE || !f->readable || off > (uint64)MAXFILE * BSIZE ||
        len > (uint64)MAXFILE * BSIZE - off)
      return -1;
    ilock(f->ip);
    int regular = f->ip->type == T_FILE;
    iunlock(f->ip);
    if (!regular)
      return -1;
  }
  for (int i = 0; i < NVMA; i++)
    if (p->vmas[i].end == 0) {
      slot = i;
      break;
    }
  if (slot < 0)
    return -1;

  // First fit, independent of descriptor order; freed holes are reusable.
  start = PGROUNDUP(p->sz);
  if (start < MMAPBASE)
    start = MMAPBASE;
  for (;;) {
    uint64 next = start;
    if (start + len > MMAPEND)
      return -1;
    for (int i = 0; i < NVMA; i++) {
      struct vma *v = &p->vmas[i];
      if (start < v->end && start + len > v->start && v->end > next)
        next = v->end;
    }
    if (next == start)
      break;
    start = next;
  }
  if (f)
    filedup(f);
  acquire(&p->lock);
  p->vmas[slot] = (struct vma){start, start + len, off, prot, f};
  release(&p->lock);
  return start;
}

uint64
sys_munmap(void)
{
  uint64 start, len, end;
  struct proc *p = myproc();
  struct file *close = 0;
  int spare = -1;
  argaddr(0, &start);
  argaddr(1, &len);
  if (start % PGSIZE || len == 0 || len > MMAPMAX)
    return -1;
  len = PGROUNDUP(len);
  struct vma *v = lookup(p, start);
  if (!v || len > v->end - start)
    return -1;
  end = start + len;
  if (start > v->start && end < v->end) {
    for (int i = 0; i < NVMA; i++)
      if (p->vmas[i].end == 0) {
        spare = i;
        break;
      }
    if (spare < 0)
      return -1; // No mutation until the split can be represented.
    if (v->file)
      filedup(v->file);
  }
  acquire(&p->lock);
  uvmunmap(p->pagetable, start, len / PGSIZE, 1);
  if (start == v->start && end == v->end) {
    close = v->file;
    memset(v, 0, sizeof(*v));
  } else if (start == v->start) {
    v->offset += len;
    v->start = end;
  } else if (end == v->end) {
    v->end = start;
  } else {
    p->vmas[spare] = *v;
    p->vmas[spare].start = end;
    p->vmas[spare].offset += end - v->start;
    v->end = start;
  }
  release(&p->lock);
  if (close)
    fileclose(close);
  return 0;
}

uint64
vmafault(pagetable_t pt, uint64 va, int read)
{
  struct proc *p = myproc();
  struct vma *v;
  char *mem;
  if (!p || pt != p->pagetable || (v = lookup(p, va)) == 0 ||
      (!read && !(v->prot & PROT_WRITE)))
    return 0;
  va = PGROUNDDOWN(va);
  if (ismapped(pt, va))
    return 0;
  // All locked uaccess paths prefault file VMAs before taking their locks.
  // Fail safely if a future caller forgets that requirement.
  push_off();
  int locked = mycpu()->noff > 1;
  pop_off();
  if (v->file && locked)
    return 0;
  if ((mem = kalloc()) == 0)
    return 0;
  memset(mem, 0, PGSIZE);
  if (v->file) {
    struct inode *ip = v->file->ip;
    uint64 off = v->offset + va - v->start;
    ilock(ip);
    // The partial last page is zero-filled; whole pages beyond EOF fault.
    if (off >= ip->size) {
      iunlock(ip);
      kfree(mem);
      return 0;
    }
    uint n = ip->size - off;
    if (n > PGSIZE)
      n = PGSIZE;
    int got = readi(ip, 0, (uint64)mem, off, n);
    iunlock(ip);
    if (got != n) {
      kfree(mem);
      return 0;
    }
  }
  int perm = PTE_U | PTE_R;
  if (v->prot & PROT_WRITE)
    perm |= PTE_W;
  acquire(&p->lock);
  int r = mappages(pt, va, PGSIZE, (uint64)mem, perm);
  release(&p->lock);
  if (r < 0) {
    kfree(mem);
    return 0;
  }
  return (uint64)mem;
}

// Resolve file-backed user buffers before inode/pipe/console/wait locks.
// Only intersecting VMAs are visited, so large invalid ranges do not walk
// the entire virtual address space. Normal copyin/out still validate holes.
int
vmaprefault(uint64 addr, uint64 len, int write)
{
  struct proc *p = myproc();
  if (len == 0)
    return 0;
  if (addr >= MAXVA || len > MAXVA - addr)
    return -1;
  uint64 end = addr + len;
  for (int i = 0; i < NVMA; i++) {
    struct vma *v = &p->vmas[i];
    if (!v->file || addr >= v->end || end <= v->start)
      continue;
    if (write && !(v->prot & PROT_WRITE))
      return -1;
    uint64 a = PGROUNDDOWN(addr > v->start ? addr : v->start);
    for (; a < end && a < v->end; a += PGSIZE)
      if (!ismapped(p->pagetable, a) &&
          !vmfault(p->pagetable, p->sz, a, !write))
        return -1;
  }
  return 0;
}

int
vmacopy(struct proc *p, struct proc *child)
{
  int i;
  // No file references are acquired until every page-table copy succeeds.
  for (i = 0; i < NVMA; i++) {
    struct vma *v = &p->vmas[i];
    if (v->end &&
        uvmcopyrange(p->pagetable, child->pagetable, v->start, v->end) < 0)
      goto bad;
  }
  for (i = 0; i < NVMA; i++) {
    child->vmas[i] = p->vmas[i];
    if (child->vmas[i].file)
      filedup(child->vmas[i].file);
  }
  return 0;
bad:
  for (int j = 0; j < i; j++) {
    struct vma *v = &p->vmas[j];
    if (v->end)
      uvmunmap(child->pagetable, v->start, (v->end - v->start) / PGSIZE, 1);
  }
  return -1;
}

void
vmaclear(struct proc *p)
{
  for (int i = 0; i < NVMA; i++) {
    acquire(&p->lock);
    struct vma v = p->vmas[i];
    if (v.end)
      uvmunmap(p->pagetable, v.start, (v.end - v.start) / PGSIZE, 1);
    memset(&p->vmas[i], 0, sizeof(v));
    release(&p->lock);
    if (v.file)
      fileclose(v.file);
  }
}

// Preserve the original large lazy heap when no VMA blocks its growth.
uint64
vmaheaplimit(struct proc *p)
{
  uint64 limit = TRAPFRAME;
  for (int i = 0; i < NVMA; i++)
    if (p->vmas[i].end && p->vmas[i].start < limit)
      limit = p->vmas[i].start;
  return limit;
}
