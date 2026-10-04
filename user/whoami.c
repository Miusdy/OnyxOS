#include "kernel/types.h"
#include "user/user.h"
int
main(void)
{
  // Public names for the fixed teaching accounts; hashes stay root-only.
  int uid = getuid();
  if (uid == 0)
    printf("root\n");
  else if (uid == 1001)
    printf("alice\n");
  else if (uid == 1002)
    printf("bob\n");
  else
    printf("uid=%d\n", uid);
  exit(0);
}
