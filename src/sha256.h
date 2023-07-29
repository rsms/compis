// SHA-256
#pragma once
ASSUME_NONNULL_BEGIN

#define SHA256_CHUNK_SIZE 64

typedef struct { uintptr data[32/sizeof(uintptr)]; } sha256_t;

typedef struct SHA256 {
  u8*   hash;
  u8    chunk[SHA256_CHUNK_SIZE];
  u8*   chunk_pos;
  usize space_left;
  usize total_len;
  u32   h[8];
} SHA256;

void sha256_init(SHA256* state, sha256_t* hash_storage);
void sha256_write(SHA256* state, const void* data, usize len);
void sha256_close(SHA256* state);

void sha256_data(sha256_t* result, const void* data, usize len);

// sha256_iszero returns if sha256 is 00000000000000000000000000000000
bool sha256_iszero(const sha256_t* sha256);

ASSUME_NONNULL_END
