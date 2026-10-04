// Cross-identity controls, privileged devices/logs, and multi-page snapshots.
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/param.h"
#include "user/user.h"

static struct psinfo snapshot[NPROC];
static void
check(int yes, const char *what)
{
  if (!yes) {
    fprintf(2, "privtest: FAIL %s\n", what);
    exit(1);
  }
}
static void
reap(int pid)
{
  int status = -1;
  check(wait(&status) == pid && status == 0, "child status");
}
int
main(void)
{
  int ready[2], hold[2], victim, child;
  char byte;
  uint64 seq = 0, lost = 0;
  check(getuid() == 0, "root entry");
  check(pipe(ready) == 0 && pipe(hold) == 0, "pipes");
  victim = fork();
  check(victim >= 0, "victim fork");
  if (victim == 0) {
    close(ready[0]);
    close(hold[1]);
    check(setpgid(0, getpid()) == 0, "victim group");
    check(setgid(1001) == 0 && setuid(1001) == 0, "victim demotion");
    check(write(ready[1], "r", 1) == 1, "ready");
    check(read(hold[0], &byte, 1) == 1, "victim survived foreign signals");
    exit(0);
  }
  close(ready[1]);
  close(hold[0]);
  check(read(ready[0], &byte, 1) == 1, "wait victim");
  close(ready[0]);
  child = fork();
  check(child >= 0, "attacker fork");
  if (child == 0) {
    close(hold[1]);
    check(setpgid(0, getpid()) == 0, "attacker group");
    check(setgid(1002) == 0 && setuid(1002) == 0, "attacker demotion");
    for (int i = 0; i < 128; i++) {
      check(kill(victim) == -1, "foreign kill");
      check(signal(victim, SIGKILL) == -1, "foreign signal");
      check(killpg(victim, SIGKILL) == -1, "foreign group signal");
      check(setprio(victim, 0) == -1, "foreign priority");
      check(setpgid(victim, getpid()) == -1, "foreign group reassignment");
      check(setpgid(0, victim) == -1, "join foreign group");
      check(tcsetpgrp(victim) == -1, "foreign foreground group");
      check(klog(&byte, 1, &seq, &lost) == -1, "foreign kernel log");
      check(mknod("/home/bob/device", 1, 0) == -1, "unprivileged mknod");
      check(ttyecho(0) == -1, "unprivileged echo control");
      check(open("/etc/passwd", O_RDONLY) == -1, "account hashes private");
      check(link("/etc/passwd", "/home/bob/pw") == -1, "protected hardlink");
      check(chown("/home/bob", 1001, 1001) == -1, "unprivileged chown");
      check(open("/home/alice/new", O_CREATE | O_WRONLY) == -1,
            "foreign directory");
    }
    check(signal(getpid(), SIGCONT) == 0, "own signal");
    check(killpg(getpgid(), SIGCONT) == 0, "own group signal");
    check(setprio(getpid(), 7) == 0, "own priority");
    check(tcsetpgrp(getpgid()) == 0, "own foreground");
    check(psinfo(snapshot, NPROC) > 0, "public statistics");
    int found = 0;
    for (int i = 0; i < NPROC; i++)
      if (snapshot[i].pid == victim) {
        check(snapshot[i].uid == 1001 && snapshot[i].gid == 1001,
              "snapshot identity");
        found = 1;
      }
    check(found, "foreign stats visible");
    exit(0);
  }
  reap(child);
  child = fork();
  check(child >= 0, "same uid fork");
  if (child == 0) {
    close(hold[1]);
    check(setgid(1001) == 0 && setuid(1001) == 0, "same uid demotion");
    check(setprio(victim, 7) == 0, "same uid priority");
    check(signal(victim, SIGCONT) == 0, "same uid signal");
    check(killpg(victim, SIGCONT) == 0, "same uid group");
    int peer = fork();
    check(peer >= 0, "same uid peer");
    if (peer == 0) {
      for (;;)
        pause(100);
    }
    check(kill(peer) == 0, "same uid kill");
    int status;
    check(wait(&status) == peer, "same uid kill reaped");
    // SIGCONT can interrupt the victim's pipe read; it retries in kernel.
    exit(0);
  }
  reap(child);
  child = fork();
  check(child >= 0, "mixed group fork");
  if (child == 0) {
    close(hold[1]);
    check(setpgid(0, victim) == 0, "root creates mixed group");
    check(setgid(1002) == 0 && setuid(1002) == 0, "mixed group identity");
    check(sigaction(SIGTERM, SIG_IGN) == 0, "ignore own group signal");
    check(killpg(victim, SIGTERM) == 0, "mixed group authorized subset");
    check(signal(victim, SIGTERM) == -1, "mixed group not authority");
    exit(0);
  }
  reap(child);
  check(setprio(victim, 5) == 0, "root foreign priority");
  check(signal(victim, SIGCONT) == 0, "root foreign signal");
  check(write(hold[1], "x", 1) == 1, "release victim");
  close(hold[1]);
  reap(victim);
  check(klog(&byte, 1, &seq, &lost) >= 0, "root log");
  unlink("priv-device");
  check(mknod("priv-device", 1, 0) == 0, "root mknod");
  check(unlink("priv-device") == 0, "device cleanup");
  // Force a second snapshot page. Children block on a pipe, no timing guesses.
  check(pipe(hold) == 0 && pipe(ready) == 0, "snapshot pipe");
  int children[56];
  for (int i = 0; i < 56; i++) {
    children[i] = fork();
    check(children[i] >= 0, "snapshot fork");
    if (children[i] == 0) {
      close(hold[1]);
      close(ready[0]);
      check(write(ready[1], "r", 1) == 1, "snapshot ready");
      close(ready[1]);
      read(hold[0], &byte, 1);
      exit(0);
    }
  }
  close(hold[0]);
  close(ready[1]);
  for (int i = 0; i < 56; i++)
    check(read(ready[0], &byte, 1) == 1, "snapshot barrier");
  check(psinfo(snapshot, NPROC) > 56, "second snapshot page");
  int firstbytes = (4096 / sizeof(struct psinfo)) * sizeof(struct psinfo);
  char *partial = sbrk(firstbytes);
  check(partial != SBRK_ERROR, "partial snapshot buffer");
  check(psinfo((struct psinfo *)partial, NPROC) == -1, "second copyout fails");
  uint64 before = freemem();
  for (int i = 0; i < 64; i++) {
    check(psinfo((struct psinfo *)-1, NPROC) == -1, "snapshot copyout failure");
    check(psinfo((struct psinfo *)partial, NPROC) == -1,
          "second copyout failure");
  }
  check(freemem() == before, "snapshot failure pages released");
  check(sbrk(-firstbytes) != SBRK_ERROR, "release partial buffer");
  close(ready[0]);
  close(hold[1]);
  for (int i = 0; i < 56; i++) {
    int status;
    check(wait(&status) > 0 && status == 0, "snapshot reap");
  }
  // Same-uid kill succeeds and the child is reaped, unlike foreign kill above.
  child = fork();
  check(child >= 0, "kill fork");
  if (child == 0) {
    for (;;)
      pause(100);
  }
  check(kill(child) == 0, "root kill");
  int status;
  check(wait(&status) == child, "killed child reap");
  printf("privtest: OK\n");
  exit(0);
}
