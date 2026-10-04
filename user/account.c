#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "user/account.h"

int
account_load(struct accounts *db)
{
  struct stat st;
  int fd = open(AUTH_PATH, O_RDONLY), ok;
  if (fd < 0)
    return -1;
  ok = fstat(fd, &st) == 0 && st.type == T_FILE && st.uid == 0 &&
       st.mode == 0600 && st.nlink == 1 && st.size == sizeof(*db) &&
       read(fd, db, sizeof(*db)) == sizeof(*db);
  close(fd);
  if (!ok || db->magic != AUTH_MAGIC || db->count != AUTH_COUNT)
    return -1;
  for (int i = 0; i < AUTH_COUNT; i++) {
    struct account *a = &db->entry[i];
    if (!a->name[0] || a->name[AUTH_NAME - 1] != 0)
      return -1;
    for (int j = 0; a->name[j]; j++)
      if (a->name[j] < 'a' || a->name[j] > 'z')
        return -1;
    if ((i == 0) != (a->uid == 0) || (i == 0 && strcmp(a->name, "root")))
      return -1;
    for (int j = 0; j < i; j++)
      if (db->entry[j].uid == a->uid || !strcmp(db->entry[j].name, a->name))
        return -1;
  }
  return 0;
}
int
account_find(struct accounts *db, const char *name)
{
  for (int i = 0; i < AUTH_COUNT; i++)
    if (!strcmp(name, db->entry[i].name))
      return i;
  return -1;
}
// Drain overlong lines and reject them, never authenticate a truncated value.
int
account_line(char *buf, int cap)
{
  int n = 0, bad = 0, rc;
  char c;
  while ((rc = read(0, &c, 1)) == 1 && c != '\n') {
    if (c == 0 || n >= cap - 1)
      bad = 1;
    else
      buf[n++] = c;
  }
  buf[n] = 0;
  return rc == 1 && !bad ? n : -1;
}

// Disable echo before advertising the prompt, so an immediate response is
// already protected. Restore echo even on EOF, signal, or an overlong line.
int
account_password(const char *prompt, char *buf, int cap)
{
  if (ttyecho(0) < 0)
    return -1;
  printf("%s", prompt);
  int n = account_line(buf, cap);
  ttyecho(1);
  printf("\n");
  return n;
}
