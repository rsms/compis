#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <err.h>

typedef uint8_t  u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef size_t   usize;

int bit_get(const void* bits, usize bit) {
  return !!( ((const u8*)bits)[bit / 8] & ((u8)1u << (bit % 8)) );
}

int main(int argc, char* argv[]) {
  int minargc = 2 + (argc > 1 && strcmp(argv[1], "-n") == 0);
  if (argc < minargc || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
    fprintf(argc < minargc ? stderr : stdout,
      "usage: %s [-n] <BIT> ...\n", argv[0]);
    return argc < minargc;
  }

  static u8 bits[65536];

  argv++, argc--;

  int newline = strcmp(*argv, "-n") != 0;
  if (!newline)
    argv++, argc--;

  u32 maxbit = 8;
  u32 nbits = sizeof(bits) * 8;

  while (argc--) {
    u32 bit = (u32)atoi(*argv++);
    if (bit >= nbits)
      errx(1, "bit %u too large (max=%u)", bit, nbits-1);
    if (bit > maxbit)
      maxbit = bit;
    ((u8*)bits)[bit / 8] |= ((u8)1u << (bit % 8));
  }

  for (u32 i = 0; i < (maxbit+7)/8; i++) {
    if (i) putc(',', stdout);
    if (bits[i]) {
      printf("0x%x", bits[i]);
    } else {
      putc('0', stdout);
    }
  }

  if (newline)
    putc('\n', stdout);

  #ifdef DEBUG
    if (!newline)
      putc('\n', stdout);
    for (u32 l = 0, i = 0; i <= maxbit; i++) {
      if (l == 0) {
        for (u32 j = 0; j < 8; j++) printf(" %3u", i+j);
        putc('\n', stdout);
      }
      printf("   %c", bit_get(bits, i) ? 'x' : ' ');
      if (++l == 8)
        l = 0, putc('\n', stdout);
    }
    putc('\n', stdout);
  #endif

  return 0;
}
