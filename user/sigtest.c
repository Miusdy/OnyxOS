#include "kernel/types.h"
#include "kernel/riscv.h"
#include "kernel/param.h"
#include "kernel/psinfo.h"
#include "user/user.h"

static volatile int caught;

static void
handler(int sig)
{
  caught = sig;
  sigreturn();
}

// The parent must already be asleep in waitpg when the stop happens.
// Polling for STOPPED in the parent misses the lost-wakeup regression.
static void
stop_waiter(void)
{
  static struct psinfo procs[NPROC];
  int parent = getpid(), status, child = fork();
  if (child < 0) {
    fprintf(2, "sigtest: waiter fork failed\n");
    exit(1);
  }
  if (child == 0) {
    setpgid(0, getpid());
    int sleeping = 0;
    for (int attempt = 0; attempt < 100 && !sleeping; attempt++) {
      int n = psinfo(procs, NPROC);
      for (int i = 0; i < n; i++)
        if (procs[i].pid == parent && procs[i].state == PSTATE_SLEEPING)
          sleeping = 1;
      if (!sleeping)
        pause(1);
    }
    if (!sleeping || signal(getpid(), SIGSTOP) < 0)
      exit(1);
    exit(0);
  }
  if (setpgid(child, child) < 0 || waitpg(child, &status) != child ||
      status != -2 || killpg(child, SIGCONT) < 0 || wait(&status) != child ||
      status != 0) {
    fprintf(2, "sigtest: blocked stop waiter failed\n");
    exit(1);
  }
}

static void
stop_pipe(int writing)
{
  static struct psinfo procs[NPROC];
  char buf[2048];
  int fds[2], status;
  if (pipe(fds) < 0)
    exit(1);
  int child = fork();
  if (child < 0)
    exit(1);
  if (child == 0) {
    setpgid(0, getpid());
    memset(buf, 'p', sizeof(buf));
    if (writing) {
      close(fds[0]);
      if (write(fds[1], buf, sizeof(buf)) != sizeof(buf))
        exit(1);
    } else {
      close(fds[1]);
      if (read(fds[0], buf, 1) != 1 || buf[0] != 'p')
        exit(1);
    }
    exit(0);
  }
  close(fds[writing ? 1 : 0]);
  int sleeping = 0;
  for (int attempt = 0; attempt < 100 && !sleeping; attempt++) {
    int n = psinfo(procs, NPROC);
    for (int i = 0; i < n; i++)
      if (procs[i].pid == child && procs[i].state == PSTATE_SLEEPING)
        sleeping = 1;
    if (!sleeping)
      pause(1);
  }
  if (!sleeping || killpg(child, SIGSTOP) < 0 ||
      waitpg(child, &status) != child || status != -2 ||
      killpg(child, SIGCONT) < 0)
    goto bad;
  if (writing) {
    int got = 0, n;
    while (got < sizeof(buf) &&
           (n = read(fds[0], buf + got, sizeof(buf) - got)) > 0)
      got += n;
    if (got != sizeof(buf))
      goto bad;
    for (int i = 0; i < sizeof(buf); i++)
      if (buf[i] != 'p')
        goto bad;
  } else if (write(fds[1], "p", 1) != 1)
    goto bad;
  close(fds[writing ? 0 : 1]);
  if (wait(&status) == child && status == 0)
    return;
bad:
  fprintf(2, "sigtest: blocked pipe stop/resume failed (%d)\n", writing);
  exit(1);
}

int
main(void)
{
  int child, status;
  uint mask = 1U << SIGTERM;
  uint64 invalid[] = {MAXVA - 1, MAXVA, MAXVA + PGSIZE, (uint64)-3,
                      (uint64)&caught};

  if (sigaction(SIGTERM, SIG_DFL) < 0 || sigaction(SIGTERM, SIG_IGN) < 0 ||
      signal(getpid(), SIGTERM) < 0 || caught != 0 ||
      sigaction(SIGTERM, handler) < 0) {
    fprintf(2, "sigtest: signal disposition setup failed\n");
    exit(1);
  }
  for (int i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    if (sigaction(SIGTERM, (void (*)(int))invalid[i]) != -1) {
      fprintf(2, "sigtest: invalid handler accepted\n");
      exit(1);
    }
  }

  // Rejected addresses must leave the installed handler intact. Successful
  // signal delivery and the following fork/wait also exercise continued use.
  if (sigmask(mask) < 0 || signal(getpid(), SIGTERM) < 0 || caught != 0 ||
      sigmask(0) < 0 || caught != SIGTERM) {
    fprintf(2, "sigtest: signal mask or sigreturn failed\n");
    exit(1);
  }

  child = fork();
  if (child < 0) {
    fprintf(2, "sigtest: fork failed\n");
    exit(1);
  }
  if (child == 0) {
    pause(10);
    exit(0);
  }
  if (setpgid(child, child) < 0 || killpg(child, SIGSTOP) < 0) {
    fprintf(2, "sigtest: process group signal failed\n");
    exit(1);
  }
  for (int i = 0; i < 1000 && jobstate(child) != 2; i++)
    pause(1);
  if (jobstate(child) != 2 || waitpg(child, &status) != child || status != -2 ||
      killpg(child, SIGCONT) < 0 || wait(&status) != child || status != 0) {
    fprintf(2, "sigtest: stop, continue, or reap failed\n");
    exit(1);
  }

  stop_waiter();
  stop_pipe(0);
  stop_pipe(1);
  printf("sigtest: OK\n");
  exit(0);
}
