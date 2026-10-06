// fslimit: turn the directory and capacity limits of this file system
// into checked facts instead of numbers you have to read out of the
// source and reason about.
//
// Six checks, in the order docs/batch13-plan.md lists them:
//
//   1. a directory keeps working past its ten direct blocks, i.e. once
//      entries live in the indirect block (entry 641 onwards)
//   2. the ceiling those numbers imply is derived from the headers
//      rather than assumed -- 16-byte entries, 64 per block, 266 blocks
//      per file, so 17,024 entries
//   3. running out of inodes fails without leaving the name behind, and
//      the inodes come back when the files are removed
//   4. running out of data blocks fails without leaving the directory
//      inconsistent, and the blocks come back when the files are removed
//   5. names at DIRSIZ and beyond: 14 characters is a name, 15 is that
//      name truncated, so two 15-character names that differ only in the
//      cut-off character are one entry -- and a path at MAXPATH is
//      refused outright rather than truncated
//   6. the free-block and free-inode counts are back where they started
//
// Usage: fslimit [n]
//
//   n is the number of entries built for check 1 (default 650, which
//   crosses the direct/indirect boundary at 641).  Raising it is
//   expensive: dirlink() looks the name up and then scans again for a
//   free slot, so building a directory costs a full pass per entry and
//   n entries cost O(n^2).  n = 700 measured 63 s in CI against a 120 s
//   per-case timeout, so the default keeps better than 2x room; n = 650
//   already means well over half a million 16-byte reads.
//
//   For the same reason the 17,024-entry ceiling is checked as
//   arithmetic plus a measured rate, not by creating 17,024 entries.
//
// Everything this program creates it also removes, so the image is left
// as it was found; check 6 is what verifies that.

#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/stat.h"
#include "user/user.h"

enum { N_DEFAULT = 650, N_LEVELS = 16 };

// The user stack is one page, so every buffer lives in .bss.
static char workdir[2 * MAXPATH];
static char scratch[2 * MAXPATH];
static char scratch2[2 * MAXPATH];
static char linkdir[2 * MAXPATH];
static char blockdir[2 * MAXPATH];
static char levels[N_LEVELS][2 * MAXPATH];
static char num[16];
static char buf[BSIZE];
static int checks;

static void
append(char *dst, const char *src)
{
  strcpy(dst + strlen(dst), src);
}

static void
decimal(char *dst, int v)
{
  char tmp[12];
  int n = 0;

  if (v == 0) {
    strcpy(dst, "0");
    return;
  }
  while (v > 0) {
    tmp[n++] = '0' + v % 10;
    v = v / 10;
  }
  for (int i = 0; i < n; i++)
    dst[i] = tmp[n - 1 - i];
  dst[n] = 0;
}

// Five zero-padded digits: xv6's printf has no field widths, and five is
// enough to name every entry the 17,024-entry ceiling allows.
static void
index4(char *dst, int v)
{
  dst[0] = '0' + (v / 10000) % 10;
  dst[1] = '0' + (v / 1000) % 10;
  dst[2] = '0' + (v / 100) % 10;
  dst[3] = '0' + (v / 10) % 10;
  dst[4] = '0' + v % 10;
  dst[5] = 0;
}

static void
sub(char *dst, const char *dir, const char *name)
{
  strcpy(dst, dir);
  append(dst, "/");
  append(dst, name);
}

static void
entry(char *dst, const char *dir, const char *prefix, int i)
{
  strcpy(dst, dir);
  append(dst, "/");
  append(dst, prefix);
  index4(num, i);
  append(dst, num);
}

// The host harness treats one upper-case word as a guest failure, so
// every complaint here says BAD instead.
static void
expect(int held, const char *what)
{
  if (held) {
    checks++;
    return;
  }
  printf("fslimit: BAD %s\n", what);
  exit(1);
}

// 2. The ceiling, as arithmetic that has to agree with the headers.
static void
check_ceiling(void)
{
  uint64 per_block = BSIZE / sizeof(struct dirent);
  uint64 first_indirect = (uint64)NDIRECT * BSIZE / sizeof(struct dirent);
  uint64 ceiling = (uint64)MAXFILE * BSIZE / sizeof(struct dirent);

  expect(sizeof(struct dirent) == (uint64)16, "a directory entry is 16 bytes");
  expect(per_block == (uint64)64, "a block holds 64 directory entries");
  expect((uint64)MAXFILE == (uint64)266, "a file is at most 266 blocks");
  expect(first_indirect == (uint64)640, "the first 640 entries are direct");
  expect(ceiling == (uint64)17024, "the directory ceiling is 17,024 entries");
  printf("fslimit: ceiling  %ld entries = %ld blocks x %ld per block\n",
         ceiling, (uint64)MAXFILE, per_block);
}

// 1. A directory past its direct blocks.
static void
check_directory(int n)
{
  struct stat st, tg;
  uint64 target_ino, size;
  uint t0, t1;
  int i, fd, first_indirect, sample[4];

  sub(linkdir, workdir, "links");
  expect(mkdir(linkdir) == 0, "mkdir the directory to grow");

  sub(scratch, linkdir, "target");
  fd = open(scratch, O_CREATE | O_WRONLY);
  expect(fd >= 0, "create the link target");
  expect(write(fd, "x", 1) == 1, "write to the link target");
  close(fd);
  expect(stat(scratch, &tg) == 0, "stat the link target");
  target_ino = tg.ino;

  t0 = uptime();
  for (i = 0; i < n; i++) {
    entry(scratch, linkdir, "e", i);
    sub(scratch2, linkdir, "target");
    if (link(scratch2, scratch) != 0) {
      printf("fslimit: BAD link %d of %d returned -1\n", i, n);
      exit(1);
    }
  }
  t1 = uptime();

  expect(stat(linkdir, &st) == 0, "stat the grown directory");
  // ".", "..", the target, and the n links.
  expect(st.size == (uint64)(n + 3) * sizeof(struct dirent),
         "the directory holds n + 3 entries");
  expect(st.size > (uint64)NDIRECT * BSIZE,
         "the directory grew past its direct blocks");
  printf("fslimit: directory %d entries, %ld bytes, %d ticks\n", n, st.size,
         t1 - t0);

  // The first entry to land in the indirect block is the one whose byte
  // offset reaches the end of the ten direct blocks.  Check both sides
  // of that line, plus both ends.
  first_indirect = (int)((uint64)NDIRECT * BSIZE / sizeof(struct dirent)) - 3;
  expect(first_indirect > 0 && first_indirect < n,
         "the direct/indirect boundary is inside the directory");
  sample[0] = 0;
  sample[1] = first_indirect - 1;
  sample[2] = first_indirect;
  sample[3] = n - 1;
  for (i = 0; i < 4; i++) {
    entry(scratch, linkdir, "e", sample[i]);
    expect(stat(scratch, &st) == 0, "stat a directory entry");
    expect(st.ino == target_ino, "the entry names the target inode");
    expect(st.nlink == n + 1, "the target has n + 1 links");
  }
  printf("fslimit: boundary  entry %d is the first indirect one\n",
         first_indirect);

  // Removing an entry in the middle clears its slot.  unlink does not
  // shrink the file, so the directory keeps its size -- and the next
  // link must reuse the cleared slot rather than append.
  expect(stat(linkdir, &st) == 0, "stat before removing an entry");
  size = st.size;
  entry(scratch, linkdir, "e", n / 2);
  expect(unlink(scratch) == 0, "unlink an entry in the middle");
  expect(stat(scratch, &st) < 0, "the removed entry is gone");
  sub(scratch, linkdir, "reused");
  sub(scratch2, linkdir, "target");
  expect(link(scratch2, scratch) == 0, "link into the cleared slot");
  expect(stat(linkdir, &st) == 0, "stat after reusing the slot");
  expect(st.size == size, "the cleared slot was reused, not appended to");

  // Now empty it: N entries plus the target plus the re-linked one.
  for (i = 0; i < n; i++) {
    entry(scratch, linkdir, "e", i);
    unlink(scratch);
  }
  sub(scratch, linkdir, "reused");
  expect(unlink(scratch) == 0, "unlink the entry that reused the slot");
  sub(scratch, linkdir, "target");
  expect(unlink(scratch) == 0, "unlink the target");
  expect(stat(linkdir, &st) == 0, "stat the emptied directory");
  expect(st.size == size, "emptying the directory did not shrink it");

  // Every slot is clear, so the next entry fits without growing the file.
  sub(scratch, linkdir, "again");
  fd = open(scratch, O_CREATE | O_WRONLY);
  expect(fd >= 0, "create a file in the emptied directory");
  close(fd);
  expect(stat(linkdir, &st) == 0, "stat after refilling one slot");
  expect(st.size == size, "the cleared slot was reused again");
  expect(unlink(scratch) == 0, "unlink that file");
  expect(unlink(linkdir) == 0, "remove the directory");
}

// 5. Names at and past DIRSIZ, and paths at MAXPATH.
static void
check_names(void)
{
  struct stat a, b, d0, d1;
  int fd, depth;

  // 14 characters: an ordinary name.
  sub(scratch, workdir, "aaaaaaaaaaaaaa");
  fd = open(scratch, O_CREATE | O_WRONLY);
  expect(fd >= 0, "create a 14-character name");
  expect(write(fd, "Q", 1) == 1, "write through the 14-character name");
  close(fd);
  expect(stat(workdir, &d0) == 0, "stat the working directory");
  expect(stat(scratch, &a) == 0, "stat the 14-character name");

  // 15 characters: skipelem() copies DIRSIZ bytes and stops, so this is
  // the same name, create() finds the file that is already there, and
  // the append lands in that file.
  sub(scratch, workdir, "aaaaaaaaaaaaaaX");
  fd = open(scratch, O_CREATE | O_WRONLY | O_APPEND);
  expect(fd >= 0, "open the 15-character alias");
  expect(write(fd, "R", 1) == 1, "append through the 15-character alias");
  close(fd);

  sub(scratch2, workdir, "aaaaaaaaaaaaaa");
  expect(stat(scratch2, &b) == 0, "stat the 14-character name again");
  expect(b.ino == a.ino, "the 15-character name is the same entry");
  expect(b.size == 2, "the append landed in the 14-character file");
  expect(stat(workdir, &d1) == 0, "stat the working directory again");
  expect(d1.size == d0.size, "the alias did not add a directory entry");

  // A second 15-character name, differing only in the character that was
  // cut off, is the same entry too.
  sub(scratch, workdir, "aaaaaaaaaaaaaaZ");
  expect(stat(scratch, &b) == 0, "stat a second 15-character name");
  expect(b.ino == a.ino, "both 15-character names are the same entry");

  // One character shorter is a different file.
  sub(scratch, workdir, "aaaaaaaaaaaaa");
  fd = open(scratch, O_CREATE | O_WRONLY);
  expect(fd >= 0, "create a 13-character name");
  close(fd);
  expect(stat(scratch, &b) == 0, "stat the 13-character name");
  expect(b.ino != a.ino, "a shorter name is a different file");
  expect(b.size == 0, "the shorter name is its own, empty, file");
  expect(unlink(scratch) == 0, "unlink the 13-character name");

  // Read the two bytes back through the original name.
  sub(scratch2, workdir, "aaaaaaaaaaaaaa");
  fd = open(scratch2, O_RDONLY);
  expect(fd >= 0, "open the 14-character name for reading");
  expect(read(fd, buf, 2) == 2, "read two bytes back");
  expect(buf[0] == 'Q' && buf[1] == 'R', "both writes are in one file");
  close(fd);

  // A single component longer than MAXPATH is refused, not truncated:
  // the path never reaches skipelem() at all.
  strcpy(scratch, "/");
  for (int i = 0; i < MAXPATH + 20; i++)
    append(scratch, "b");
  expect(open(scratch, O_CREATE | O_WRONLY) < 0,
         "a path longer than MAXPATH is refused");
  expect(stat(scratch, &b) < 0, "a path longer than MAXPATH does not resolve");

  // Nesting, to find where MAXPATH actually bites: keep going while the
  // next level still fits, and check that the level after that does not.
  depth = 0;
  strcpy(scratch, workdir);
  while (depth < N_LEVELS) {
    strcpy(scratch2, scratch);
    append(scratch2, "/dddddddddddddd");
    if (strlen(scratch2) >= MAXPATH) {
      expect(mkdir(scratch2) < 0, "a path at MAXPATH is refused");
      break;
    }
    expect(mkdir(scratch2) == 0, "mkdir one level deeper");
    strcpy(levels[depth], scratch2);
    strcpy(scratch, scratch2);
    depth++;
  }
  expect(depth > 1 && depth < N_LEVELS, "nesting stopped at the path limit");
  printf("fslimit: names     deepest path %d bytes, the next level refused\n",
         (int)strlen(scratch));
  for (int i = depth - 1; i >= 0; i--)
    expect(unlink(levels[i]) == 0, "remove a nested directory");

  sub(scratch2, workdir, "aaaaaaaaaaaaaa");
  expect(unlink(scratch2) == 0, "unlink the 14-character file");
  printf("fslimit: names     14 and 15 characters are one entry\n");
}

// 3. Inodes.
static void
check_inodes(void)
{
  struct fsstat before, after;
  struct stat st;
  int i, fd, made = 0;

  expect(fsinfo(&before) == 0, "fsinfo before the inode check");

  for (i = 0; i < 1000; i++) {
    entry(scratch, workdir, "i", i);
    if ((fd = open(scratch, O_CREATE | O_WRONLY)) < 0)
      break;
    close(fd);
    made++;
  }
  expect(made < 1000, "the file system runs out of inodes");
  expect(made > 0, "some files were created before it ran out");

  // The name that failed must not be there, and creation must keep
  // failing: the failure is clean, not a half-written entry.
  expect(stat(scratch, &st) < 0, "the failed name does not exist");
  expect(open(scratch, O_CREATE | O_WRONLY) < 0, "creation keeps failing");

  // What was created before the failure still works.
  entry(scratch2, workdir, "i", made - 1);
  expect(stat(scratch2, &st) == 0, "the last file created is still there");

  for (i = 0; i < made; i++) {
    entry(scratch, workdir, "i", i);
    expect(unlink(scratch) == 0, "unlink a file from the inode check");
  }

  expect(fsinfo(&after) == 0, "fsinfo after the inode check");
  expect(after.inodesfree >= before.inodesfree, "every inode came back");
  printf("fslimit: inodes    ran out after %d files, %ld free again\n", made,
         after.inodesfree);
}

// 4. Data blocks, and what a full disk does to a directory.
static void
check_blocks(void)
{
  struct fsstat before, after;
  struct stat st;
  uint64 off;
  int i, fd, made = 0, entries = 0, full = 0;

  sub(blockdir, workdir, "bl");
  expect(mkdir(blockdir) == 0, "mkdir the block-check directory");
  sub(scratch, blockdir, "target");
  fd = open(scratch, O_CREATE | O_WRONLY);
  expect(fd >= 0, "create the block-check target");
  close(fd);

  // Stop when the directory is a whole number of blocks, so that the next
  // entry cannot fit in the block that is already there and needs a fresh
  // one.  Without that the failure below might just be a full last block.
  for (i = 0; i < 200; i++) {
    entry(scratch, blockdir, "f", i);
    sub(scratch2, blockdir, "target");
    expect(link(scratch2, scratch) == 0, "link into the block-check directory");
    entries++;
    expect(stat(blockdir, &st) == 0, "stat the block-check directory");
    if (st.size % BSIZE == 0)
      break;
  }
  expect(st.size % BSIZE == 0, "the directory ends on a block boundary");
  expect(st.size == BSIZE, "the directory is exactly one block");
  printf("fslimit: blocks    directory pinned at %ld bytes\n", st.size);

  expect(fsinfo(&before) == 0, "fsinfo before filling the disk");

  // Fill the free space.  A file stops at MAXFILE, so the interesting
  // failure is the one that happens before a file gets there.
  for (i = 0; i < 64 && !full; i++) {
    entry(scratch, workdir, "big", i);
    if ((fd = open(scratch, O_CREATE | O_WRONLY)) < 0) {
      full = 1;
      break;
    }
    made++;
    off = 0;
    while (off < (uint64)MAXFILE * BSIZE) {
      int m = write(fd, buf, (int)sizeof(buf));
      if (m != (int)sizeof(buf)) {
        full = 1;
        break;
      }
      off += m;
    }
    close(fd);
  }
  expect(full, "the disk filled up");
  expect(made < 64, "it took fewer than 64 files to fill the disk");
  printf("fslimit: blocks    %d files filled the disk\n", made);

  // The disk is full, so a new directory entry cannot get a block.  It
  // has to fail cleanly: no entry, and the directory left as it was.
  sub(scratch, blockdir, "overflow");
  expect(open(scratch, O_CREATE | O_WRONLY) < 0,
         "a new entry is refused when the disk is full");
  expect(stat(scratch, &st) < 0, "the refused entry was not created");
  expect(stat(blockdir, &st) == 0, "stat the directory again");
  expect(st.size == BSIZE, "the refused create did not change the directory");
  entry(scratch, blockdir, "f", 0);
  expect(stat(scratch, &st) == 0,
         "an entry still resolves when the disk is full");
  sub(scratch, blockdir, "target");
  expect(stat(scratch, &st) == 0, "the target still resolves when full");

  // Everything the fill took has to come back.
  for (i = 0; i < made; i++) {
    entry(scratch, workdir, "big", i);
    expect(unlink(scratch) == 0, "unlink a file that filled the disk");
  }
  expect(fsinfo(&after) == 0, "fsinfo after removing the fillers");
  expect(after.blocksfree >= before.blocksfree, "the data blocks came back");
  printf("fslimit: blocks    %ld free before, %ld after\n", before.blocksfree,
         after.blocksfree);

  for (i = 0; i < entries; i++) {
    entry(scratch, blockdir, "f", i);
    expect(unlink(scratch) == 0, "unlink an entry from the block check");
  }
  sub(scratch, blockdir, "target");
  expect(unlink(scratch) == 0, "unlink the block-check target");
  expect(unlink(blockdir) == 0, "remove the block-check directory");
}

// 6. Nothing was leaked.
static void
check_totals(struct fsstat *base, struct sysinfo *si0)
{
  struct fsstat fin;
  struct sysinfo si1;

  sync();
  expect(fsinfo(&fin) == 0, "fsinfo at the end");
  printf("fslimit: totals    blocks %ld used, %ld free (started with %ld)\n",
         fin.blocks - fin.blocksfree, fin.blocksfree, base->blocksfree);
  expect(fin.blocksfree >= base->blocksfree, "no data blocks were lost");
  expect(fin.inodesfree >= base->inodesfree, "no inodes were lost");

  expect(sysinfo(&si1) == 0, "sysinfo at the end");
  // Only reported, not asserted: this program's heap is still mapped, so
  // pages_live is expected to be higher than it was at the start.  Page
  // accounting is kmemtest's job.
  printf("fslimit: totals    pages_live %ld at the start, %ld at the end\n",
         si0->pages_live, si1.pages_live);
}

int
main(int argc, char *argv[])
{
  struct fsstat base;
  struct sysinfo si0;
  struct stat st;
  int n = N_DEFAULT;

  if (argc > 1) {
    n = atoi(argv[1]);
    // Below 645 the directory never reaches its indirect block, which is
    // the point of check 1; the argument is for going bigger, not smaller.
    if (n < 645 || n > 99999) {
      fprintf(2, "fslimit: n must be between 645 and 99999\n");
      exit(1);
    }
  }

  expect(fsinfo(&base) == 0, "fsinfo at the start");
  expect(sysinfo(&si0) == 0, "sysinfo at the start");

  // A name of its own, so a second run in the same image does not trip
  // over the first one's leftovers.
  strcpy(workdir, "/fslimit");
  decimal(num, getpid());
  append(workdir, num);
  expect(mkdir(workdir) == 0, "mkdir the working directory");
  expect(stat(workdir, &st) == 0, "stat the working directory");
  expect(st.type == T_DIR, "the working directory is a directory");

  check_ceiling();
  check_directory(n);
  check_names();
  check_inodes();
  check_blocks();

  // The working directory is empty but keeps the size it grew to.
  expect(unlink(workdir) == 0, "remove the working directory");

  check_totals(&base, &si0);

  printf("fslimit: OK (%d checks)\n", checks);
  exit(0);
}
