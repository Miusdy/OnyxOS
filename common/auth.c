// SHA-256 and PBKDF2-HMAC-SHA256, one 32-byte output block. Shared by mkfs
// and user space. test-auth.py checks the implementation against hashlib.
#include "kernel/types.h"
#include "common/auth.h"

struct sha {
  uint h[8];
  uint bytes, used;
  uchar block[64];
};
static const uint k[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
  0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
  0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
  0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
  0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
  0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
  0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
  0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
  0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
static uint
rr(uint x, int n)
{
  return (x >> n) | (x << (32 - n));
}
static void
transform(struct sha *s)
{
  uint w[64], a, b, c, d, e, f, g, h, t1, t2;
  for (int i = 0; i < 16; i++)
    w[i] = ((uint)s->block[4 * i] << 24) | ((uint)s->block[4 * i + 1] << 16) |
           ((uint)s->block[4 * i + 2] << 8) | s->block[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    uint x = w[i - 15], y = w[i - 2];
    w[i] = w[i - 16] + (rr(x, 7) ^ rr(x, 18) ^ (x >> 3)) + w[i - 7] +
           (rr(y, 17) ^ rr(y, 19) ^ (y >> 10));
  }
  a = s->h[0];
  b = s->h[1];
  c = s->h[2];
  d = s->h[3];
  e = s->h[4];
  f = s->h[5];
  g = s->h[6];
  h = s->h[7];
  for (int i = 0; i < 64; i++) {
    t1 = h + (rr(e, 6) ^ rr(e, 11) ^ rr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] +
         w[i];
    t2 = (rr(a, 2) ^ rr(a, 13) ^ rr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  s->h[0] += a;
  s->h[1] += b;
  s->h[2] += c;
  s->h[3] += d;
  s->h[4] += e;
  s->h[5] += f;
  s->h[6] += g;
  s->h[7] += h;
}
static void
init(struct sha *s)
{
  const uint iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  for (int i = 0; i < 8; i++)
    s->h[i] = iv[i];
  s->bytes = s->used = 0;
}
static void
update(struct sha *s, const uchar *p, uint n)
{
  s->bytes += n;
  while (n--) {
    s->block[s->used++] = *p++;
    if (s->used == 64) {
      transform(s);
      s->used = 0;
    }
  }
}
static void
finish(struct sha *s, uchar *out)
{
  uint bits = s->bytes * 8;
  uchar byte = 0x80;
  update(s, &byte, 1);
  byte = 0;
  while (s->used != 56)
    update(s, &byte, 1);
  for (int i = 0; i < 4; i++)
    update(s, &byte, 1);
  for (int i = 3; i >= 0; i--) {
    byte = bits >> (8 * i);
    update(s, &byte, 1);
  }
  for (int i = 0; i < 32; i++)
    out[i] = s->h[i / 4] >> (24 - 8 * (i % 4));
}
static void
hmac(const char *password, const uchar *data, uint n, uchar *out)
{
  uchar pad[64], inner[32];
  struct sha s;
  uint len = 0;
  while (password[len] && len < AUTH_PASS - 1)
    len++;
  for (int i = 0; i < 64; i++)
    pad[i] = (i < len ? password[i] : 0) ^ 0x36;
  init(&s);
  update(&s, pad, 64);
  update(&s, data, n);
  finish(&s, inner);
  for (int i = 0; i < 64; i++)
    pad[i] ^= 0x36 ^ 0x5c;
  init(&s);
  update(&s, pad, 64);
  update(&s, inner, 32);
  finish(&s, out);
}
void
auth_hash(const char *password, const uchar *salt, uchar *out)
{
  uchar first[20], u[32];
  for (int i = 0; i < 16; i++)
    first[i] = salt[i];
  first[16] = first[17] = first[18] = 0;
  first[19] = 1;
  hmac(password, first, 20, u);
  for (int i = 0; i < 32; i++)
    out[i] = u[i];
  for (int j = 1; j < AUTH_ROUNDS; j++) {
    hmac(password, u, 32, u);
    for (int i = 0; i < 32; i++)
      out[i] ^= u[i];
  }
}
int
auth_equal(const uchar *a, const uchar *b)
{
  uint different = 0;
  for (int i = 0; i < 32; i++)
    different |= a[i] ^ b[i];
  return different == 0;
}
