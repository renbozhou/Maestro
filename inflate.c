/*
 * Copyright (c) 2020 Micha Mettke
 *
 * I reformatted the code according to my coding style.
 *
 * Copyright (C) 2020  Edward LEI <edward_lei72@hotmail.com>
 *
 * The code is licensed under the MIT license
 */

#include <string.h> /* memcpy, memset */
#include "inflate.h"


typedef struct _inflate inflate_t;

struct _inflate {
  int bits, bitcnt;
  unsigned lits[288];
  unsigned dsts[32];
  unsigned lens[19];
  int tlit, tdist, tlen;
};

static const unsigned char inflate_mirror[256] = {
  #define R2(n) n, n + 128, n + 64, n + 192
  #define R4(n) R2(n), R2(n + 32), R2(n + 16), R2(n + 48)
  #define R6(n) R4(n), R4(n +  8), R4(n +  4), R4(n + 12)
  R6(0), R6(2), R6(1), R6(3),
};

static int
_get(const unsigned char* *src, const unsigned char* end, inflate_t* s, int n)
{
  const unsigned char* in = *src;
  int v = s->bits & ((1 << n) - 1);
  s->bits >>= n;
  s->bitcnt = s->bitcnt - n;
  s->bitcnt = s->bitcnt < 0 ? 0 : s->bitcnt;
  while (s->bitcnt < 16 && in < end) {
    s->bits |= (*in++) << s->bitcnt;
    s->bitcnt += 8;
  }

  *src = in;
  return v;
}

static int
_build(unsigned* tree, unsigned char* lens, int symcnt)
{
  int n, cnt[16] = {0}, first[16], codes[16];
  cnt[0] = first[0] = codes[0] = 0;
  for (n = 0; n < symcnt; ++n) cnt[lens[n]]++;
  for (n = 1; n <= 15; n++) {
      codes[n] = (codes[n - 1] + cnt[n - 1]) << 1;
      first[n] = first[n - 1] + cnt[n - 1];
  }
  for (n = 0; n < symcnt; n++) {
    int slot, code, len = lens[n];
    if (!len) continue;
    code = codes[len]++;
    slot = first[len]++;
    tree[slot] = (unsigned)((code << (32 - len)) | (n << 4) | len);
  }

  return first[15];
}

static int
_decode(const unsigned char* *in, const unsigned char* end, inflate_t* s, unsigned* tree, int max)
{
  /* bsearch next prefix code */
  #define _rev16(n) ((inflate_mirror[(n) & 0xff] << 8) | inflate_mirror[((n) >> 8) & 0xff])
  unsigned key, lo = 0, hi = (unsigned)max;
  unsigned search = (unsigned)(_rev16(s->bits) << 16) | 0xffff;
  while (lo < hi) {
    unsigned guess = (lo + hi) / 2;
    if (search < tree[guess]) hi = guess;
    else lo = guess + 1;
  }
  /* pull out and check key */
  key = tree[lo - 1];
  _get(in, end, s, key & 0x0f);
  return (key >> 4) & 0x0fff;
}

int
inflate(unsigned char* out, const unsigned char* in, int size)
{
  static const unsigned char order[] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
  };
  static const short dbase[30 + 2] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
    257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
  };
  static const unsigned char dbits[30 + 2] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,
    10,10,11,11,12,12,13,13,0,0
  };
  static const short lbase[29 + 2] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,
    43,51,59,67,83,99,115,131,163,195,227,258,0,0
  };
  static const unsigned char lbits[29 + 2] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0,0,0
  };

  const unsigned char* e = in + size, *o = out;
  enum inflate_states {hdr,stored,fixed,dyn,blk};
  enum inflate_states state = hdr;
  struct _inflate s = {0};
  int last = 0;

  _get(&in,e,&s,0); /* buffer input */
  while (in < e || s.bitcnt) {
    switch (state) {
      case hdr: {
        int type = 0; /* block header */
        last = _get(&in, e, &s, 1);
        type = _get(&in, e, &s, 2);

        switch (type) {
        default: return (int)(out - o);
        case 0x00: state = stored; break;
        case 0x01: state = fixed; break;
        case 0x02: state = dyn; break;}
      }
      break;

      case stored: {
        int len; /* uncompressed block */
        //int nlen;
        _get(&in, e, &s, s.bitcnt & 7);
        len = _get(&in, e, &s, 16);
        //nlen = _get(&in, e, &s, 16);
        in -= 2; s.bitcnt = 0;

        if (len > (e-in) || !len) return (int)(out - o);
        memcpy(out, in, (size_t)len);
        in += len, out += len;
        state = hdr;
      }
      break;

      case fixed: {
        /* fixed huffman codes */
        int n; unsigned char lens[288 + 32];
        for (n = 0; n <= 143; n++) lens[n] = 8;
        for (n = 144; n <= 255; n++) lens[n] = 9;
        for (n = 256; n <= 279; n++) lens[n] = 7;
        for (n = 280; n <= 287; n++) lens[n] = 8;
        for (n = 0; n < 32; n++) lens[288 + n] = 5;

        /* build trees */
        s.tlit  = _build(s.lits, lens, 288);
        s.tdist = _build(s.dsts, lens + 288, 32);
        state = blk;
      }
      break;

      case dyn: {
        /* dynamic huffman codes */
        int n, i, nlit, ndist, nlen;
        unsigned char nlens[19] = {0}, lens[288 + 32];
        nlit = 257 + _get(&in, e, &s, 5);
        ndist = 1 + _get(&in, e, &s, 5);
        nlen = 4 + _get(&in, e, &s, 4);
        for (n = 0; n < nlen; n++) {
          nlens[order[n]] = (unsigned char)_get(&in, e, &s, 3);
        }
        s.tlen = _build(s.lens, nlens, 19);

        /* decode code lengths */
        for (n = 0; n < nlit + ndist;) {
          int sym = _decode(&in, e, &s, s.lens, s.tlen);
          switch (sym) {
            default:
              lens[n++] = (unsigned char)sym;
            break;

            case 16:
              for (i = 3 + _get(&in,e,&s,2); i; i--,n++) {
                lens[n] = lens[n - 1];
              }
            break;

            case 17:
              for (i = 3 + _get(&in,e,&s,3); i; i--,n++) {
                lens[n] = 0;
              }
            break;

            case 18:
              for (i = 11 + _get(&in,e,&s,7); i; i--,n++) {
                lens[n] = 0;
              }
            break;
          }
        }
        /* build lit/dist trees */
        s.tlit  = _build(s.lits, lens, nlit);
        s.tdist = _build(s.dsts, lens + nlit, ndist);
        state = blk;
      }
      break;

      case blk: {
        /* decompress block */
        int sym = _decode(&in, e, &s, s.lits, s.tlit);
        if (sym > 256) {
          sym -= 257; /* match symbol */
          {
            int len = _get(&in, e, &s, lbits[sym]) + lbase[sym];
            int dsym = _decode(&in, e, &s, s.dsts, s.tdist);
            int offs = _get(&in, e, &s, dbits[dsym]) + dbase[dsym];
            if (offs > (int)(out - o)) {
              return (int)(out - o);
            }
            while (len--) *out = *(out - offs), out++;
          }
        }
        else if (sym == 256) {
          if (last) return (int)(out - o);
          state = hdr;
        }
        else {
          *out++ = (unsigned char)sym;
        }
      }
      break;
    }
  }

  return (int)(out - o);
}