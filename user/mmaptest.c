#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define PAGE 4096
#define RW   (PROT_READ | PROT_WRITE)
#define ANON (MAP_PRIVATE | MAP_ANONYMOUS)

static void
check(int ok, char *what)
{
  if (!ok) {
    printf("mmaptest: FAIL %s\n", what);
    exit(1);
  }
}

static char *
anon(int pages)
{
  char *p = mmap(0, pages * PAGE, RW, ANON, -1, 0);
  check(p != MAP_FAILED, "anonymous map");
  return p;
}

static void
reap(int pid, int expected)
{
  int status = 99;
  check(pid > 0 && wait(&status) == pid && status == expected, "child status");
}

static void
badaccess(char *p, int write)
{
  int pid = fork();
  check(pid >= 0, "fork badaccess");
  if (pid == 0) {
    if (write)
      *(volatile char *)p = 1;
    else {
      volatile char c = *p;
      (void)c;
    }
    exit(42);
  }
  reap(pid, -1);
}

static void
anonymous(void)
{
  uint64 before = freemem();
  char *p = anon(1024);
  check(before - freemem() < 8 * PAGE, "reservation is lazy");
  check(p[0] == 0 && p[100 * PAGE] == 0, "zero fill");
  p[0] = 'A';
  int pid = fork();
  check(pid >= 0, "fork COW");
  if (!pid) {
    check(p[0] == 'A', "COW inherited data");
    p[0] = 'B';
    p[PAGE] = 'C'; // An unfaulted page remains private after fork.
    int fd[2];
    check(pipe(fd) == 0 && write(fd[1], "D", 1) == 1 &&
            read(fd[0], p + 100 * PAGE, 1) == 1,
          "COW kernel copyout");
    close(fd[0]);
    close(fd[1]);
    exit(0); // Automatic teardown, including untouched pages.
  }
  reap(pid, 0);
  check(p[0] == 'A' && p[PAGE] == 0 && p[100 * PAGE] == 0, "COW isolation");
  check(munmap(p, 1024 * PAGE) == 0, "unmap large reservation");
  badaccess(p, 0);
  p = anon(3);
  p[0] = 'a';
  p[2 * PAGE] = 'c';
  check(munmap(p + PAGE, PAGE) == 0, "split");
  check(p[0] == 'a' && p[2 * PAGE] == 'c', "split preserves neighbors");
  badaccess(p + PAGE, 1);
  char *hole = anon(1);
  check(hole == p + PAGE && hole[0] == 0, "reuse hole with zero fill");
  check(munmap(hole, PAGE) == 0 && munmap(p, PAGE) == 0 &&
          munmap(p + 2 * PAGE, PAGE) == 0,
        "split cleanup");
  p = mmap(0, PAGE, PROT_READ, ANON, -1, 0);
  check(p != MAP_FAILED && p[0] == 0, "readonly anonymous");
  badaccess(p, 1);
  check(munmap(p, PAGE) == 0, "readonly cleanup");
}

static void
invalid(void)
{
  check(mmap(0, 0, RW, ANON, -1, 0) == MAP_FAILED, "zero length");
  check(mmap(0, (uint64)-1, RW, ANON, -1, 0) == MAP_FAILED, "overflow");
  check(mmap((void *)PAGE, PAGE, RW, ANON, -1, 0) == MAP_FAILED,
        "address hint");
  check(mmap(0, PAGE, 0, ANON, -1, 0) == MAP_FAILED, "PROT_NONE");
  check(mmap(0, PAGE, PROT_WRITE, ANON, -1, 0) == MAP_FAILED, "write only");
  check(mmap(0, PAGE, 7, ANON, -1, 0) == MAP_FAILED, "executable mapping");
  check(mmap(0, PAGE, RW, 0, -1, 0) == MAP_FAILED, "invalid flags");
  check(mmap(0, PAGE, RW, ANON, 0, 0) == MAP_FAILED, "anonymous fd");
  check(mmap(0, PAGE, RW, ANON, -1, PAGE) == MAP_FAILED, "anonymous offset");
  check(mmap(0, PAGE, RW, MAP_PRIVATE, -1, 0) == MAP_FAILED, "bad fd");
  char *p = anon(3);
  char *brk = sbrk(0);
  int gap = (uint64)p - (uint64)brk;
  check(sbrklazy(gap + PAGE) == SBRK_ERROR && sbrk(0) == brk,
        "heap rejects overlap without changing break");
  check(sbrklazy(gap) == brk && sbrk(0) == p,
        "heap can reach mapping boundary");
  check(sbrk(-gap) == p, "restore break");
  check(munmap(p + 1, PAGE) == -1 && munmap(p, 0) == -1 &&
          munmap(p, (uint64)-1) == -1 && munmap(p, 4 * PAGE) == -1,
        "invalid unmap leaves mapping intact");
  p[0] = 7;
  char *slots[NVMA - 1];
  for (int i = 0; i < NVMA - 1; i++)
    slots[i] = anon(1);
  check(mmap(0, PAGE, RW, ANON, -1, 0) == MAP_FAILED, "table full");
  check(munmap(p + PAGE, PAGE) == -1, "split rollback on table full");
  p[PAGE] = 8;
  check(p[0] == 7, "full table preserves data");
  for (int i = 0; i < NVMA - 1; i++)
    check(munmap(slots[i], PAGE) == 0, "slots cleanup");
  check(munmap(p, PAGE) == 0 && munmap(p + 2 * PAGE, 1) == 0 &&
          munmap(p + PAGE, PAGE) == 0,
        "prefix suffix rounded unmap");
  check(munmap(p, PAGE) == -1, "double unmap");
}

static char data[PAGE];

static void
files(void)
{
  unlink("mmap.data");
  int fd = open("mmap.data", O_CREATE | O_RDWR);
  check(fd >= 0, "create file");
  memset(data, 'a', sizeof(data));
  check(write(fd, data, PAGE) == PAGE, "write first page");
  memset(data, 'b', sizeof(data));
  check(write(fd, data, PAGE) == PAGE && write(fd, "tail", 4) == 4,
        "write tail");
  close(fd);
  fd = open("mmap.data", O_RDONLY);
  check(fd >= 0, "open readonly");
  check(mmap(0, PAGE, RW, MAP_PRIVATE, fd, 1) == MAP_FAILED,
        "unaligned offset");
  check(mmap(0, PAGE, RW, MAP_PRIVATE, fd, (uint64)-PAGE) == MAP_FAILED,
        "offset overflow");
  char *p = mmap(0, 4 * PAGE, RW, MAP_PRIVATE, fd, 0);
  check(p != MAP_FAILED, "private write allowed on readable fd");
  char *q = mmap(0, PAGE, PROT_READ, MAP_PRIVATE, fd, PAGE);
  check(q != MAP_FAILED, "offset mapping");
  close(fd);
  check(p[0] == 'a' && q[0] == 'b', "demand read after close");
  check(p[2 * PAGE] == 't' && p[2 * PAGE + 4] == 0 && p[3 * PAGE - 1] == 0,
        "EOF tail zero");
  badaccess(p + 3 * PAGE, 0);
  badaccess(q, 1);
  p[0] = 'x';
  int pid = fork();
  check(pid >= 0, "file fork");
  if (!pid) {
    check(p[0] == 'x' && p[PAGE] == 'b', "resident and lazy file fork");
    p[0] = 'y';
    exit(0);
  }
  reap(pid, 0);
  check(p[0] == 'x', "file COW isolation");
  check(munmap(p, PAGE) == 0 && p[PAGE] == 'b', "prefix adjusts file offset");
  check(munmap(p + 2 * PAGE, 2 * PAGE) == 0 && munmap(p + PAGE, PAGE) == 0 &&
          munmap(q, PAGE) == 0,
        "file cleanup");
  fd = open("mmap.data", O_RDWR);
  check(read(fd, data, 1) == 1 && data[0] == 'a',
        "private writes not persisted");
  close(fd);

  // Same inode I/O into a cold mapping must not recursively lock that inode.
  fd = open("mmap.data", O_RDWR);
  p = mmap(0, PAGE, RW, MAP_PRIVATE, fd, PAGE);
  check(p != MAP_FAILED && read(fd, p, 1) == 1 && p[0] == 'a',
        "cold file read buffer");
  check(munmap(p, PAGE) == 0, "read buffer cleanup");
  p = mmap(0, PAGE, RW, MAP_PRIVATE, fd, PAGE);
  check(p != MAP_FAILED && write(fd, p, 1) == 1,
        "cold same-inode write buffer");
  check(munmap(p, PAGE) == 0, "write buffer cleanup");
  p = mmap(0, PAGE, RW, MAP_PRIVATE, fd, PAGE);
  int pp[2];
  check(pipe(pp) == 0 && write(pp[1], p, 1) == 1 && read(pp[0], data, 1) == 1 &&
          data[0] == 'b',
        "cold pipe source");
  check(munmap(p, PAGE) == 0, "pipe source cleanup");
  p = mmap(0, PAGE, RW, MAP_PRIVATE, fd, PAGE);
  check(write(pp[1], "z", 1) == 1 && read(pp[0], p, 1) == 1 && p[0] == 'z',
        "cold pipe destination");
  close(pp[0]);
  close(pp[1]);
  check(munmap(p, PAGE) == 0, "pipe destination cleanup");
  p = mmap(0, PAGE, RW, MAP_PRIVATE, fd, 0);
  pid = fork();
  check(pid >= 0, "wait buffer fork");
  if (!pid)
    exit(37);
  check(wait((int *)p) == pid && *(int *)p == 37, "cold wait output");
  check(munmap(p, PAGE) == 0, "wait buffer cleanup");
  close(fd);
  for (int i = 0; i < 120; i++) {
    fd = open("mmap.data", O_RDONLY);
    check(fd >= 0, "mapping file reference leak");
    p = mmap(0, 3 * PAGE, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    check(p != MAP_FAILED && munmap(p + PAGE, PAGE) == 0 && p[2 * PAGE] == 't',
          "file split keeps offset");
    check(munmap(p, PAGE) == 0 && munmap(p + 2 * PAGE, PAGE) == 0,
          "file split drops references");
  }
  fd = open("mmap.data", O_WRONLY);
  check(mmap(0, PAGE, PROT_READ, MAP_PRIVATE, fd, 0) == MAP_FAILED,
        "writeonly fd");
  close(fd);
  fd = open("/", O_RDONLY);
  check(mmap(0, PAGE, PROT_READ, MAP_PRIVATE, fd, 0) == MAP_FAILED,
        "directory fd");
  close(fd);

  // An unlinked backing inode must survive until the last mapping reference.
  fd = open("mmap.data", O_RDONLY);
  p = mmap(0, PAGE, PROT_READ, MAP_PRIVATE, fd, PAGE);
  close(fd);
  check(unlink("mmap.data") == 0 && p[0] == 'b', "unlinked file lifetime");
  check(munmap(p, PAGE) == 0, "last mapping releases inode");
}

static void
lifecycle(void)
{
  uint64 before = freemem();
  for (int i = 0; i < 20; i++) {
    int pid = fork();
    check(pid >= 0, "lifecycle fork");
    if (!pid) {
      char *p = anon(16);
      for (int j = 0; j < 16; j++)
        p[j * PAGE] = j;
      int fd = open("README", O_RDONLY);
      check(fd >= 0 &&
              mmap(0, PAGE, PROT_READ, MAP_PRIVATE, fd, 0) != MAP_FAILED,
            "lifecycle file map");
      close(fd);
      if (i % 2) {
        strcpy(p, "mmaptest");
        strcpy(p + 32, "exec-check");
        char **args = (char **)(p + 128);
        args[0] = p;
        args[1] = p + 32;
        args[2] = 0;
        exec(p, args);
        check(0, "exec");
      }
      exit(0);
    }
    reap(pid, 0);
  }
  check(freemem() + 4 * PAGE >= before, "exit/exec memory leak");
}

static void
pressure(void)
{
  uint64 before = freemem();
  int pid = fork();
  check(pid >= 0, "pressure fork");
  if (!pid) {
    char *a = anon(MMAPMAX / PAGE);
    char *b = anon(MMAPMAX / PAGE);
    for (int i = 0; i < MMAPMAX; i += PAGE) {
      a[i] = 1;
      b[i] = 2;
    }
    exit(42); // 128 MiB of data cannot fit in the 128 MiB machine.
  }
  reap(pid, -1);
  check(freemem() + 4 * PAGE >= before, "OOM teardown leak");
  char *p = anon(1);
  p[0] = 3;
  check(p[0] == 3 && munmap(p, PAGE) == 0, "allocation after OOM");
}

static void
concurrent(void)
{
  for (int i = 0; i < 6; i++) {
    int pid = fork();
    check(pid >= 0, "concurrent fork");
    if (!pid) {
      for (int j = 0; j < 24; j++) {
        char *p = anon(8);
        int fd = open("README", O_RDONLY);
        char *q = mmap(0, PAGE, PROT_READ, MAP_PRIVATE, fd, 0);
        check(fd >= 0 && q != MAP_FAILED && q[0] != 0, "concurrent file fault");
        close(fd);
        for (int k = 0; k < 8; k++)
          p[k * PAGE] = k;
        check(munmap(p + PAGE, 6 * PAGE) == 0 && munmap(p, PAGE) == 0 &&
                munmap(p + 7 * PAGE, PAGE) == 0 && munmap(q, PAGE) == 0,
              "concurrent unmap");
      }
      exit(0);
    }
  }
  for (int i = 0; i < 6; i++) {
    int status;
    check(wait(&status) > 0 && status == 0, "concurrent reap");
  }
}

int
main(int argc, char **argv)
{
  if (argc == 2 && strcmp(argv[1], "exec-check") == 0) {
    check(open((char *)MMAPBASE, O_RDONLY) == -1, "exec removes old mappings");
    exit(0);
  }
  invalid();
  anonymous();
  files();
  lifecycle();
  concurrent();
  pressure();
  printf("mmaptest: OK\n");
  exit(0);
}
