// Root-only password changes for the provisioned accounts. Account salts are
// random at image creation and stable across password changes (no guest RNG).
#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "user/account.h"

int
main(int argc, char **argv)
{
  struct accounts db;
  char first[AUTH_PASS], second[AUTH_PASS];
  int fd, i;
  if (getuid() != 0 || argc != 2) {
    fprintf(2, "usage: passwd NAME (root only)\n");
    exit(1);
  }
  // An atomic directory creation serializes writers; a crash leaves a lock
  // for root to inspect/remove instead of silently accepting concurrent edits.
  if (mkdir("/etc/pwlock") < 0) {
    fprintf(2, "passwd: database busy; inspect /etc/pwlock\n");
    exit(1);
  }
  int ok = 0;
  if (account_load(&db) < 0 || (i = account_find(&db, argv[1])) < 0)
    goto done;
  if (account_password("New password: ", first, sizeof(first)) <= 0)
    goto done;
  if (account_password("Retype password: ", second, sizeof(second)) <= 0 ||
      strcmp(first, second))
    goto done;
  auth_hash(first, db.entry[i].salt, db.entry[i].hash);
  if ((fd = open(AUTH_PATH, O_WRONLY)) < 0)
    goto done;
  // Fixed length, no O_TRUNC: the log commits all bytes together.
  ok = write(fd, &db, sizeof(db)) == sizeof(db);
  close(fd);
  if (ok)
    sync();
done:
  memset(first, 0, sizeof(first));
  memset(second, 0, sizeof(second));
  memset(&db, 0, sizeof(db));
  unlink("/etc/pwlock");
  printf(ok ? "Password updated\n" : "passwd: update failed\n");
  exit(ok ? 0 : 1);
}
