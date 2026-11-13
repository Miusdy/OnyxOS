// tracetest: prove that the syscall flight recorder records the right
// process, path, arguments and return value, and that it does not leak one
// user's records to another.
//
// Run as a dedicated test (testrun tracetest) on a single-use QEMU session.
// It clears the ring before each check, so a previous run in the same image
// cannot affect the result.

#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/fcntl.h"
#include "kernel/stat.h"
#include "kernel/syscall.h"
#include "kernel/trace.h"
#include "user/user.h"

static struct trace_entry buf[TRACE_NENT];
static int checks;

static void
expect(int ok, const char *what)
{
  if (ok) {
    checks++;
    return;
  }
  printf("tracetest: BAD %s\n", what);
  exit(1);
}

static int
read_all(uint64 *lost)
{
  int n = traceread(buf, TRACE_NENT, lost);

  if (n < 0) {
    printf("tracetest: BAD traceread\n");
    exit(1);
  }
  return n;
}

static struct trace_entry *
find_exit(int n, int pid, int num)
{
  struct trace_entry *found = 0;
  int i;

  for (i = 0; i < n; i++)
    if (buf[i].flags == TRACE_EXIT && buf[i].pid == pid &&
        buf[i].syscall == num)
      found = &buf[i];
  return found;
}

// 1. A failed open records the caller, its parent, the path and the value.
static void
test_open(void)
{
  uint64 lost;
  int pid, st, n;
  struct trace_entry *e;

  expect(tracectl(TRACE_CLEAR, 0) == 0, "root can clear the ring");
  pid = fork();
  if (pid == 0) {
    if (tracectl(TRACE_ON, 0) != 0)
      exit(2);
    if (open("/no/such/file", O_RDONLY) >= 0)
      exit(3);
    exit(0);
  }
  expect(pid > 0, "fork the traced child");
  expect(wait(&st) == pid && st == 0, "the traced child exits cleanly");

  n = read_all(&lost);
  e = find_exit(n, pid, SYS_open);
  expect(e != 0, "the failed open is recorded");
  expect(e->ret == (uint64)-1, "the recorded open failed");
  expect(strcmp(e->path, "/no/such/file") == 0, "the path is recorded");
  expect(e->ppid == getpid(), "the parent pid is recorded");
  expect(e->uid == 0, "the uid is recorded");
}

// 2. Attaching by pid records another process, which is what trace -p uses.
static void
test_attach(void)
{
  uint64 lost;
  int fds[2], pid, st, n;
  char c;
  struct trace_entry *e;

  expect(tracectl(TRACE_CLEAR, 0) == 0, "clear before the attach test");
  expect(pipe(fds) == 0, "pipe for the attach test");
  pid = fork();
  if (pid == 0) {
    close(fds[1]);
    if (read(fds[0], &c, 1) != 1)
      exit(2);
    if (open("/no/such/file", O_RDONLY) >= 0)
      exit(3);
    exit(0);
  }
  expect(pid > 0, "fork the attach target");
  close(fds[0]);
  expect(tracectl(TRACE_ON, pid) == 0, "attach to another process");
  expect(write(fds[1], "x", 1) == 1, "release the attach target");
  close(fds[1]);
  expect(wait(&st) == pid && st == 0, "the attach target exits");

  n = read_all(&lost);
  e = find_exit(n, pid, SYS_open);
  expect(e != 0, "the attached process is recorded");
  expect(e->ret == (uint64)-1, "the attached open failed");
  expect(strcmp(e->path, "/no/such/file") == 0,
         "the attached path is recorded");
}

// 3. Turning tracing off stops new records.
static void
test_disable(void)
{
  uint64 lost;
  uint32 before = 0, after = 0;
  int i, n, me;

  expect(tracectl(TRACE_CLEAR, 0) == 0, "clear before the disable test");
  expect(tracectl(TRACE_ON, 0) == 0, "enable tracing on self");
  me = getpid();
  expect(tracectl(TRACE_OFF, 0) == 0, "disable tracing on self");

  n = read_all(&lost);
  for (i = 0; i < n; i++)
    if (buf[i].flags == TRACE_EXIT && buf[i].pid == me && buf[i].seq > before)
      before = buf[i].seq;
  expect(before != 0, "the getpid was recorded");

  for (i = 0; i < 5; i++)
    getpid();
  n = read_all(&lost);
  for (i = 0; i < n; i++)
    if (buf[i].flags == TRACE_EXIT && buf[i].pid == me && buf[i].seq > after)
      after = buf[i].seq;
  expect(after == before, "the ring stops growing while tracing is off");
}

// 4. trace -f covers a grandchild: the record's pid and ppid place it one
//    level below the process that trace itself started.
static void
test_follow(void)
{
  uint64 lost;
  int i, pid, st, n, helper, seen;
  struct trace_entry *e;

  expect(tracectl(TRACE_CLEAR, 0) == 0, "clear before the follow test");
  pid = fork();
  if (pid == 0) {
    char *args[] = {"/trace", "-f", "/tracetest", "child", 0};
    exec("/trace", args);
    exit(127);
  }
  expect(pid > 0, "fork the trace command");
  expect(wait(&st) == pid && st == 0, "trace -f finishes the target");

  n = read_all(&lost);
  e = 0;
  for (i = 0; i < n; i++)
    if (buf[i].flags == TRACE_EXIT && buf[i].syscall == SYS_open &&
        strcmp(buf[i].path, "/no/such/file") == 0)
      e = &buf[i];
  expect(e != 0, "the grandchild's failed open is recorded");
  expect(e->pid != pid, "the record is not from the direct child");
  expect(e->ppid != getpid(), "the record is not from a child of the test");
  expect(e->ppid != pid, "the record comes from a grandchild");

  helper = e->ppid;
  seen = 0;
  for (i = 0; i < n; i++)
    if (buf[i].pid == helper && buf[i].flags == TRACE_EXIT)
      seen = 1;
  expect(seen, "the intermediate process was traced");
}

// 5. Permissions: a non-root process may trace itself, not clear the ring,
//    not attach to root, and never sees root's records.
static void
test_permissions(void)
{
  uint64 lost;
  int i, parent, pid, st, n;

  expect(tracectl(TRACE_CLEAR, 0) == 0, "clear before the permission test");
  parent = getpid();
  pid = fork();
  if (pid == 0) {
    if (setuid(1) != 0)
      exit(10);
    if (tracectl(TRACE_CLEAR, 0) != -1)
      exit(11);
    if (tracectl(TRACE_ON, parent) != -1)
      exit(12);
    if (tracectl(TRACE_ON, 0) != 0)
      exit(13);
    getpid();
    n = traceread(buf, 16, &lost);
    if (n <= 0)
      exit(14);
    for (i = 0; i < n; i++)
      if (buf[i].uid != 1)
        exit(15);
    if (tracectl(TRACE_OFF, 0) != 0)
      exit(16);
    exit(0);
  }
  expect(wait(&st) == pid && st == 0, "non-root tracing rules");

  expect(tracectl(TRACE_CLEAR, 0) == 0, "clear before the filter test");
  expect(tracectl(TRACE_ON, 0) == 0, "trace root on self");
  getpid();
  expect(tracectl(TRACE_OFF, 0) == 0, "trace root off self");

  pid = fork();
  if (pid == 0) {
    if (setuid(1) != 0)
      exit(20);
    n = traceread(buf, 64, &lost);
    if (n != 0)
      exit(21);
    exit(0);
  }
  expect(wait(&st) == pid && st == 0, "other-uid records are filtered");
}

// 6. Wrap-around: the newest window is still contiguous and lost grows.
static void
test_wrap(void)
{
  uint64 lost1 = 0, lost2 = 0;
  uint32 first, last;
  int i, n;

  expect(tracectl(TRACE_CLEAR, 0) == 0, "clear before the wrap test");
  expect(tracectl(TRACE_ON, 0) == 0, "trace self for the wrap test");
  for (i = 0; i < 1300; i++)
    getpid();

  n = traceread(buf, TRACE_NENT, &lost1);
  expect(n == TRACE_NENT, "a full ring is returned");
  expect(lost1 > 0, "the ring wrapped");
  first = buf[0].seq;
  last = buf[n - 1].seq;
  expect(last > first, "records are in write order");
  expect(last - first + 1 == (uint32)n, "the window is contiguous");

  for (i = 0; i < 20; i++)
    getpid();
  n = traceread(buf, TRACE_NENT, &lost2);
  expect(n == TRACE_NENT, "the ring stays full");
  expect(lost2 > lost1, "lost records keep increasing");
  expect(tracectl(TRACE_OFF, 0) == 0, "stop tracing after the wrap test");
}

// Helper mode for test_follow: run under trace, then fork one more level so
// the failed open comes from a grandchild of the trace command.
static void
child_mode(void)
{
  int pid, st;

  pid = fork();
  if (pid == 0) {
    if (open("/no/such/file", O_RDONLY) >= 0)
      exit(1);
    exit(0);
  }
  if (pid < 0 || wait(&st) != pid || st != 0)
    exit(1);
  exit(0);
}

int
main(int argc, char **argv)
{
  if (argc > 1 && strcmp(argv[1], "child") == 0)
    child_mode();

  test_open();
  test_attach();
  test_disable();
  test_follow();
  test_permissions();
  test_wrap();

  printf("tracetest: OK (%d checks)\n", checks);
  exit(0);
}
