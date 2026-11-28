// sleep: pause for a whole number of seconds.
//
// There is no RTC and no wall clock here; the timer tick is about 100 ms and
// pause(n) waits n ticks.  So sleep is a thin user-level wrapper rather than
// a new system call: seconds are converted to ticks in this file.

#include "kernel/types.h"
#include "user/user.h"

#define TICKS_PER_SEC 10
#define MAX_SECONDS   86400

// Require every character to be a digit and reject values above the limit.
static int
parse_seconds(const char *s, int *out)
{
  int v = 0;

  if (*s == 0)
    return -1;
  for (; *s; s++) {
    if (*s < '0' || *s > '9')
      return -1;
    if (v > (MAX_SECONDS - (*s - '0')) / 10)
      return -1;
    v = v * 10 + (*s - '0');
  }
  *out = v;
  return 0;
}

int
main(int argc, char *argv[])
{
  int secs;

  if (argc != 2) {
    fprintf(2, "usage: sleep seconds\n");
    exit(2);
  }
  if (parse_seconds(argv[1], &secs) < 0) {
    fprintf(2, "sleep: bad seconds: %s\n", argv[1]);
    exit(2);
  }
  uint duration = secs * TICKS_PER_SEC;
  uint start = uptime();
  uint elapsed = 0;
  // A stop or continue can interrupt pause(). Count time across job control
  // using unsigned tick differences so an uptime counter wrap is harmless.
  while (elapsed < duration && pause(duration - elapsed) < 0)
    elapsed = (uint)uptime() - start;
  exit(0);
}
