// Shared definitions for the syscall flight recorder (batch 15).
//
// This header is included by the kernel and by user space, so the record
// layout must be identical in both worlds.  It must be included after a
// header that defines uint64/ushort/uchar (kernel/types.h), like
// kernel/psinfo.h.
//
// traceread() copies records straight into a user buffer, so changing
// struct trace_entry changes the ABI between kernel/trace.c and user/trace.c.

#ifndef XV6_TRACE_H
#define XV6_TRACE_H

#include "kernel/syscall.h"

// The ring holds this many records; traceread() never returns more.
#define TRACE_NENT 1024

// A path is captured into a fixed field so that recording a syscall can
// never fail.  A longer path is truncated; an unreadable one is left empty.
#define TRACE_PATH 32

// tracectl() operations.
#define TRACE_OFF   0
#define TRACE_ON    1
#define TRACE_CLEAR 2

// Entry flags: a syscall produces an ENTER record with its arguments and
// path, and an EXIT record with the return value.
#define TRACE_ENTER 1
#define TRACE_EXIT  2

// Where the record came from.  Syscalls are the only producer today; the
// field leaves room for kernel-side trace points without a layout change.
#define TRACE_MODE_SYSCALL 0

struct trace_entry {
  uint64 tick;    // process CPU ticks (user + kernel) at the record
  uint64 ret;     // return value; 0 for an ENTER record
  uint64 args[3]; // first three syscall arguments
  int pid;
  int ppid;
  ushort uid;
  ushort syscall;
  uchar mode;            // TRACE_MODE_*
  uchar flags;           // TRACE_ENTER or TRACE_EXIT
  uint32 seq;            // write sequence; readers use it to spot a rewrite
  char path[TRACE_PATH]; // path argument for path-taking calls, else ""
};

// One table, two expansions: kernel/trace.c and user/trace.c both build
// their name lookup from this list, so a new syscall is named once.
#define TRACE_SYSCALLS(X)                                                      \
  X(SYS_fork, "fork")                                                          \
  X(SYS_exit, "exit")                                                          \
  X(SYS_wait, "wait")                                                          \
  X(SYS_pipe, "pipe")                                                          \
  X(SYS_read, "read")                                                          \
  X(SYS_kill, "kill")                                                          \
  X(SYS_exec, "exec")                                                          \
  X(SYS_fstat, "fstat")                                                        \
  X(SYS_chdir, "chdir")                                                        \
  X(SYS_dup, "dup")                                                            \
  X(SYS_getpid, "getpid")                                                      \
  X(SYS_sbrk, "sbrk")                                                          \
  X(SYS_pause, "pause")                                                        \
  X(SYS_uptime, "uptime")                                                      \
  X(SYS_open, "open")                                                          \
  X(SYS_write, "write")                                                        \
  X(SYS_mknod, "mknod")                                                        \
  X(SYS_unlink, "unlink")                                                      \
  X(SYS_link, "link")                                                          \
  X(SYS_mkdir, "mkdir")                                                        \
  X(SYS_close, "close")                                                        \
  X(SYS_sync, "sync")                                                          \
  X(SYS_psinfo, "psinfo")                                                      \
  X(SYS_freemem, "freemem")                                                    \
  X(SYS_klog, "klog")                                                          \
  X(SYS_waitx, "waitx")                                                        \
  X(SYS_fsinfo, "fsinfo")                                                      \
  X(SYS_sysinfo, "sysinfo")                                                    \
  X(SYS_setprio, "setprio")                                                    \
  X(SYS_sigaction, "sigaction")                                                \
  X(SYS_sigmask, "sigmask")                                                    \
  X(SYS_sigreturn, "sigreturn")                                                \
  X(SYS_signal, "signal")                                                      \
  X(SYS_killpg, "killpg")                                                      \
  X(SYS_setpgid, "setpgid")                                                    \
  X(SYS_getpgid, "getpgid")                                                    \
  X(SYS_tcsetpgrp, "tcsetpgrp")                                                \
  X(SYS_waitpg, "waitpg")                                                      \
  X(SYS_jobstate, "jobstate")                                                  \
  X(SYS_getuid, "getuid")                                                      \
  X(SYS_getgid, "getgid")                                                      \
  X(SYS_setuid, "setuid")                                                      \
  X(SYS_setgid, "setgid")                                                      \
  X(SYS_chmod, "chmod")                                                        \
  X(SYS_chown, "chown")                                                        \
  X(SYS_ttyecho, "ttyecho")                                                    \
  X(SYS_mmap, "mmap")                                                          \
  X(SYS_munmap, "munmap")                                                      \
  X(SYS_tracectl, "tracectl")                                                  \
  X(SYS_traceread, "traceread")

#endif // XV6_TRACE_H
