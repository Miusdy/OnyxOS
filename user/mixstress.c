#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "kernel/stat.h"
#include "user/user.h"

#define WORKERS 6
#define ROUNDS  12
#define BYTES   8192
static char original[BYTES], received[BYTES];

static void
check(int ok, char *what)
{
  if (!ok) {
    fprintf(2, "mixstress: FAIL %s\n", what);
    exit(1);
  }
}

static void
worker(int id)
{
  char name[] = "mix0";
  name[3] += id;
  for (int r = 0; r < ROUNDS; r++) {
    int p[2], status, fd;
    memset(original, 'a' + id, BYTES);
    check(pipe(p) == 0, "pipe");
    int pid = fork();
    check(pid >= 0, "fork");
    if (pid == 0) {
      close(p[0]);
      memset(original, 'A' + id, BYTES);
      check(write(p[1], original, BYTES) == BYTES, "pipe write");
      close(p[1]);
      fd = open(name, O_CREATE | O_RDWR | O_TRUNC);
      check(fd >= 0, "create");
      check(write(fd, original, BYTES) == BYTES, "file write");
      close(fd);
      exit(0);
    }
    close(p[1]);
    int n = 0, got;
    while ((got = read(p[0], received + n, BYTES - n)) > 0)
      n += got;
    check(n == BYTES, "pipe length");
    close(p[0]);
    check(wait(&status) == pid && status == 0, "child status");
    for (int i = 0; i < BYTES; i++)
      check(original[i] == 'a' + id && received[i] == 'A' + id,
            "COW isolation/pipe data");
    fd = open(name, O_RDONLY);
    check(fd >= 0, "open");
    check(read(fd, received, BYTES) == BYTES, "file read");
    for (int i = 0; i < BYTES; i++)
      check(received[i] == 'A' + id, "file data");
    close(fd);
    check(unlink(name) == 0, "unlink");
  }
  exit(0);
}

// Independent opens must select EOF while holding the inode lock. Each
// short write is one record; verify no lost, torn, or duplicated records.
static void
appendtest(void)
{
  enum { RECORDS = 32, SIZE = 64 };
  int ready[2], go[2], fd, status;
  char record[SIZE], token;
  char seen[WORKERS][RECORDS];
  memset(seen, 0, sizeof(seen));
  fd = open("mixappend", O_CREATE | O_WRONLY | O_TRUNC);
  check(fd >= 0 && write(fd, "seed", 4) == 4, "append seed");
  close(fd);
  check(pipe(ready) == 0 && pipe(go) == 0, "append barriers");
  for (int id = 0; id < WORKERS; id++) {
    int pid = fork();
    check(pid >= 0, "append fork");
    if (pid == 0) {
      close(ready[0]);
      close(go[1]);
      fd = open("mixappend", O_WRONLY | O_APPEND);
      check(fd >= 0, "append open");
      check(write(ready[1], "r", 1) == 1 && read(go[0], &token, 1) == 1,
            "append barrier");
      for (int r = 0; r < RECORDS; r++) {
        memset(record, 'a' + id, sizeof(record));
        record[0] = id;
        record[1] = r;
        check(write(fd, record, sizeof(record)) == sizeof(record),
              "append record");
      }
      close(fd);
      exit(0);
    }
  }
  close(ready[1]);
  close(go[0]);
  for (int i = 0; i < WORKERS; i++)
    check(read(ready[0], &token, 1) == 1, "append ready");
  for (int i = 0; i < WORKERS; i++)
    check(write(go[1], "g", 1) == 1, "append start");
  close(ready[0]);
  close(go[1]);
  for (int i = 0; i < WORKERS; i++)
    check(wait(&status) > 0 && status == 0, "append child");
  fd = open("mixappend", O_RDONLY);
  check(fd >= 0 && read(fd, record, 4) == 4 && memcmp(record, "seed", 4) == 0,
        "append preserves prefix");
  for (int i = 0; i < WORKERS * RECORDS; i++) {
    check(read(fd, record, SIZE) == SIZE, "append count");
    int id = (uchar)record[0], r = (uchar)record[1];
    check(id < WORKERS && r < RECORDS && !seen[id][r], "append unique");
    seen[id][r] = 1;
    for (int j = 2; j < SIZE; j++)
      check(record[j] == 'a' + id, "append intact");
  }
  check(read(fd, record, 1) == 0, "append EOF");
  close(fd);
  // Reopening, dup, and writes beyond one transaction keep append mode.
  fd = open("mixappend", O_RDWR | O_APPEND);
  check(fd >= 0 && read(fd, record, 4) == 4, "append read offset");
  int other = dup(fd);
  close(fd);
  memset(original, 'z', BYTES);
  check(other >= 0 && write(other, original, BYTES) == BYTES,
        "append large dup write");
  struct stat st;
  check(fstat(other, &st) == 0 &&
          st.size == 4 + WORKERS * RECORDS * SIZE + BYTES,
        "append large size");
  close(other);
  check(unlink("mixappend") == 0, "append unlink");
}

int
main(void)
{
  struct fsstat before, after;
  appendtest();
  // Warm up the root directory slots before recording block usage.
  for (int i = 0; i < WORKERS; i++) {
    char name[] = "mix0";
    name[3] += i;
    int fd = open(name, O_CREATE | O_RDWR);
    check(fd >= 0, "warmup");
    close(fd);
  }
  for (int i = 0; i < WORKERS; i++) {
    char name[] = "mix0";
    name[3] += i;
    check(unlink(name) == 0, "warmup unlink");
  }
  check(fsinfo(&before) == 0, "fsinfo");
  for (int i = 0; i < WORKERS; i++) {
    int pid = fork();
    check(pid >= 0, "worker fork");
    if (pid == 0)
      worker(i);
  }
  for (int i = 0; i < WORKERS; i++) {
    int status;
    check(wait(&status) > 0 && status == 0, "worker status");
  }
  check(wait(0) == -1, "unreaped child");
  sync();
  check(fsinfo(&after) == 0, "fsinfo after");
  check(before.blocksfree == after.blocksfree &&
          before.inodesfree == after.inodesfree,
        "filesystem leak");
  printf("mixstress: OK (%d workers, %d rounds)\n", WORKERS, ROUNDS);
  exit(0);
}
