// Process information exported to user space for observability
// (the ps/top programs).  This header is included by BOTH kernel and
// user code, so the layout must be identical in both worlds.  It relies
// on fixed-width types (int = 4 bytes, uint64 = 8 bytes on rv64) and on
// explicit padding, and it must be included after a header that defines
// uint64 (kernel/types.h or user/user.h via kernel/types.h).

#ifndef XV6_PSINFO_H
#define XV6_PSINFO_H

// Must equal sizeof(((struct proc *)0)->name) in kernel/proc.h.
#define PINFO_NAME 16

// Scheduling priority, in the spirit of nice(1): a smaller number is more
// urgent.  The kernel clamps setprio() to this range, and the aging pass in
// the scheduler keeps even PRIO_LOWEST from starving.
#define PRIO_HIGHEST 0
#define PRIO_LOWEST  9
#define PRIO_DEFAULT 5

// Mirror of enum procstate in kernel/proc.h.  Kept as plain #defines so
// user code does not need to include kernel/proc.h (which drags in the
// whole kernel process structure).
#define PSTATE_UNUSED   0
#define PSTATE_USED     1
#define PSTATE_SLEEPING 2
#define PSTATE_RUNNABLE 3
#define PSTATE_RUNNING  4
#define PSTATE_STOPPED  5
#define PSTATE_ZOMBIE   6

struct psinfo {
  ushort uid;
  ushort gid;
  uint _pad;
  int pid;               // process id
  int ppid;              // parent process id (0 if none)
  int state;             // one of the PSTATE_* values above
  int prio;              // scheduling priority, smaller is more urgent
  uint64 sz;             // virtual memory size in bytes, NOT resident set size
  uint64 rss;            // resident set size in bytes (pages actually mapped)
  uint64 u_ticks;        // timer ticks charged while in user mode
  uint64 k_ticks;        // timer ticks charged while in supervisor mode
  char name[PINFO_NAME]; // process name, always NUL-terminated
};

// 72 bytes per entry. The kernel snapshots into two independently allocated
// pages, then copies packed entries into the user's buffer. Statistics are
// public across users; kernel logs are restricted to uid 0.

#endif // XV6_PSINFO_H
