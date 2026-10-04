#include "kernel/types.h"
#include "user/user.h"
#include "user/account.h"

// pause() may return early on a signal even when its handler is SIG_IGN.
// Retry the remaining interval so Ctrl-C cannot bypass authentication delay.
static void
delay(uint ticks)
{
  uint start = uptime(), elapsed;
  while ((elapsed = (uint)uptime() - start) < ticks)
    pause(ticks - elapsed);
}

int
main(void)
{
  struct accounts db;
  char name[AUTH_NAME], password[AUTH_PASS];
  uchar hash[32];
  int failures = 0;
  if (getuid() != 0) {
    fprintf(2, "login: requires root\n");
    exit(1);
  }
  setpgid(0, getpid());
  tcsetpgrp(getpgid());
  // Keep failure backoff in this process; terminal interrupts cannot reset it.
  sigaction(SIGINT, SIG_IGN);
  sigaction(SIGTSTP, SIG_IGN);
  for (;;) {
    if (account_load(&db) < 0) {
      fprintf(2, "login: invalid account database\n");
      delay(50);
      continue;
    }
    printf("login: ");
    int valid = account_line(name, sizeof(name)) > 0;
    valid &= account_password("Password: ", password, sizeof(password)) > 0;
    int i = account_find(&db, name);
    // Unknown users pay the same hash cost and receive the same error.
    struct account *a = &db.entry[i < 0 ? 0 : i];
    auth_hash(password, a->salt, hash);
    memset(password, 0, sizeof(password));
    if (!valid || i < 0 || !auth_equal(hash, a->hash)) {
      failures = failures < 4 ? failures + 1 : 4;
      printf("Login incorrect\n");
      delay(10 << (failures - 1)); // 1, 2, 4, then 8 seconds, bounded.
      continue;
    }
    int uid = a->uid, gid = a->gid;
    memset(&db, 0, sizeof(db));
    memset(hash, 0, sizeof(hash));
    // Drop group first, then uid. No privileged account descriptor survives.
    if (setgid(gid) < 0 || setuid(uid) < 0)
      exit(1);
    char *args[] = {"sh", 0};
    exec("/sh", args);
    fprintf(2, "login: exec sh failed\n");
    exit(1);
  }
}
