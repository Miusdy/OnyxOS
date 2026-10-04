#ifndef MINIOS_AUTH_H
#define MINIOS_AUTH_H

// Fixed-size, little-endian account database. Fits in one filesystem block;
// passwd rewrites it in one write transaction without truncating the inode.
#define AUTH_MAGIC  0x41555431U
#define AUTH_COUNT  3
#define AUTH_NAME   16
#define AUTH_PASS   64
#define AUTH_ROUNDS 10000
#define AUTH_PATH   "/etc/passwd"
struct account {
  char name[AUTH_NAME];
  ushort uid, gid;
  uchar salt[16];
  uchar hash[32];
};
struct accounts {
  uint magic, count;
  struct account entry[AUTH_COUNT];
};
void auth_hash(const char *, const uchar *, uchar *);
int auth_equal(const uchar *, const uchar *);
#endif
