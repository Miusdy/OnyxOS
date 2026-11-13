//
// formatted console output -- printk, panic.
//

#include <stdarg.h>

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

volatile int panicking = 0; // printing a panic message
volatile int panicked = 0;  // spinning forever at end of a panic

// lock to avoid interleaving concurrent printk's.
static struct {
  struct spinlock lock;
} pr;

static char digits[] = "0123456789abcdef";

// Kernel log ring buffer: a copy of everything printk emits, so that
// user space can replay boot messages and later diagnostics.
//
// The writer publishes the byte first and only then bumps the write
// index with a release store, so a reader that acquire-loads the index
// is guaranteed to see a fully written byte.  It is deliberately
// lock-free: panic() must be able to log without taking any lock.
//
// Two writers racing (possible on the panic path) can lose a byte, but
// klog_w only ever increases by one, so an index is never out of range
// and the buffer is never corrupted.
#define KLOGSIZE 16384 // must be a power of two
static char klogbuf[KLOGSIZE];
static uint64 klog_w; // bytes ever written; byte i lives at i % KLOGSIZE

static void
klog_putc(int c)
{
  uint64 w = __atomic_load_n(&klog_w, __ATOMIC_RELAXED);

  klogbuf[w % KLOGSIZE] = c;
  __atomic_store_n(&klog_w, w + 1, __ATOMIC_RELEASE);
}

// Emit one character to both the console and the kernel log.  The tee
// lives here, in printk.c, rather than in consputc() so that console
// input echo (see console.c) is not captured by the log.
static void
kputc(int c)
{
  klog_putc(c);
  consputc(c);
}

static void
printint(long long xx, int base, int sign)
{
  char buf[20];
  int i;
  unsigned long long x;

  if (sign && (sign = (xx < 0)))
    x = -xx;
  else
    x = xx;

  i = 0;
  do {
    buf[i++] = digits[x % base];
  } while ((x /= base) != 0);

  if (sign)
    buf[i++] = '-';

  while (--i >= 0)
    kputc(buf[i]);
}

static void
printptr(uint64 x)
{
  int i;
  kputc('0');
  kputc('x');
  for (i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
    kputc(digits[x >> (sizeof(uint64) * 8 - 4)]);
}

// Print to the console.
int
printk(char *fmt, ...)
{
  va_list ap;
  int i, cx, c0, c1, c2;
  char *s;

  if (panicking == 0)
    acquire(&pr.lock);

  va_start(ap, fmt);
  for (i = 0; (cx = fmt[i] & 0xff) != 0; i++) {
    if (cx != '%') {
      kputc(cx);
      continue;
    }
    i++;
    c0 = fmt[i + 0] & 0xff;
    c1 = c2 = 0;
    if (c0)
      c1 = fmt[i + 1] & 0xff;
    if (c1)
      c2 = fmt[i + 2] & 0xff;
    if (c0 == 'd') {
      printint(va_arg(ap, int), 10, 1);
    } else if (c0 == 'l' && c1 == 'd') {
      printint(va_arg(ap, uint64), 10, 1);
      i += 1;
    } else if (c0 == 'l' && c1 == 'l' && c2 == 'd') {
      printint(va_arg(ap, uint64), 10, 1);
      i += 2;
    } else if (c0 == 'u') {
      printint(va_arg(ap, uint32), 10, 0);
    } else if (c0 == 'l' && c1 == 'u') {
      printint(va_arg(ap, uint64), 10, 0);
      i += 1;
    } else if (c0 == 'l' && c1 == 'l' && c2 == 'u') {
      printint(va_arg(ap, uint64), 10, 0);
      i += 2;
    } else if (c0 == 'x') {
      printint(va_arg(ap, uint32), 16, 0);
    } else if (c0 == 'l' && c1 == 'x') {
      printint(va_arg(ap, uint64), 16, 0);
      i += 1;
    } else if (c0 == 'l' && c1 == 'l' && c2 == 'x') {
      printint(va_arg(ap, uint64), 16, 0);
      i += 2;
    } else if (c0 == 'p') {
      printptr(va_arg(ap, uint64));
    } else if (c0 == 'c') {
      kputc(va_arg(ap, uint));
    } else if (c0 == 's') {
      if ((s = va_arg(ap, char *)) == 0)
        s = "(null)";
      for (; *s; s++)
        kputc(*s);
    } else if (c0 == '%') {
      kputc('%');
    } else if (c0 == 0) {
      break;
    } else {
      // Print unknown % sequence to draw attention.
      kputc('%');
      kputc(c0);
    }
  }
  va_end(ap);

  if (panicking == 0)
    release(&pr.lock);

  return 0;
}

// Copy up to max bytes of the kernel log to user address uva.
//
// *seq is an in/out cursor: on entry it holds the absolute index of the
// first byte the caller wants (pass 0 for "as far back as the ring
// still goes"); on return it holds the index just past the last byte
// copied.  *lost is set to the number of bytes that were overwritten
// before the caller's cursor, i.e. that the caller will never see.
//
// No lock is held across copyout, and a concurrent writer can only
// clobber bytes we are copying if the ring wraps during the copy; in
// that case the tail of the output is a harmless mix of old and new
// log text.  The log is diagnostic output, not a reliable channel.
int
klog_read(pagetable_t pagetable, uint64 psz, uint64 uva, int max, uint64 *seq,
          uint64 *lost)
{
  uint64 w, s, start, n, i, first;

  *lost = 0;
  if (max < 0)
    return -1;

  w = __atomic_load_n(&klog_w, __ATOMIC_ACQUIRE);
  s = *seq;

  // Clamp the cursor up to the oldest byte still present in the ring.
  if (w > KLOGSIZE) {
    start = w - KLOGSIZE;
    if (s < start) {
      *lost = start - s;
      s = start;
    }
  }
  if (s > w) // stale or bogus cursor: nothing to report
    s = w;

  n = w - s;
  if (n > (uint64)max)
    n = (uint64)max;
  if (n == 0) {
    *seq = s;
    return 0;
  }

  // Copy in at most two pieces to handle a buffer wrap.
  i = s % KLOGSIZE;
  first = KLOGSIZE - i;
  if (first > n)
    first = n;
  if (copyout(pagetable, psz, uva, &klogbuf[i], first) < 0)
    return -1;
  if (n > first && copyout(pagetable, psz, uva + first, klogbuf, n - first) < 0)
    return -1;

  *seq = s + n;
  return n;
}

void
panic(char *s)
{
  panicking = 1;
  printk("panic: ");
  printk("%s\n", s);
  // Last chance to keep the syscall trail: only the current process, only
  // if it asked to be traced, and only reading the ring (no locks, no
  // allocation).  A partial dump is marked as such by trace_panic_dump().
  trace_panic_dump(myproc());
  panicked = 1; // freeze uart output from other CPUs
  for (;;)
    ;
}

void
printkinit(void)
{
  initlock(&pr.lock, "pr");
}
