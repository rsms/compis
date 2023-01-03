// growable byte buffer (specialization of array)
// SPDX-License-Identifier: Apache-2.0
// 
// Example
//   buf_t b;
//   buf_init(&b, ma);
//   for (usize i = 0; i < 1024; i++)
//     assert(buf_push(&b, 'a'));
//   buf_dispose(&b);
//
#pragma once
ASSUME_NONNULL_BEGIN

typedef struct {
  union {
    u8*   nullable bytes;
    char* nullable chars;
    void* nullable p;
  };
  usize cap, len;
  memalloc_t ma;
  bool       external; // true if p is external storage, not managed by ma
} buf_t;

// note: buf_t is castable to both mem_t and slice_t

// buf_init initializes a buffer (sets v, cap, len to zero and sets ma to ma.)
// buf_initext initializes a buffer with external storage
void buf_init(buf_t* b, memalloc_t ma);
void buf_initext(buf_t* b, memalloc_t ma, void* p, usize cap);

// buf_make returns an initialized zero-capacity buffer
// buf_makeext returns a buffer referencing initial external storage
inline static buf_t buf_make(memalloc_t ma) { return (buf_t){ .ma = ma }; }
inline static buf_t buf_makeext(memalloc_t ma, void* p, usize cap) {
  return (buf_t){ .p = p, .cap = cap, .ma = ma, .external = true };
}

// buf_dispose frees memory.
// b remains valid after this call, as if buf_init(b,b->ma) was called.
void buf_dispose(buf_t* b);

// buf_avail returns the number of available bytes to write without growing the buffer
inline static usize buf_avail(const buf_t* b) { return b->cap - b->len; }

// buf_clear empties the buffer
inline static void buf_clear(buf_t* b) { b->len = 0; }

// buf_slice returns a slice of a buffer
// 1. buf_slice(const buf_t b)
// 2. buf_slice(const buf_t b, usize start, usize len)
#define buf_slice(...) __VARG_DISP(_buf_slice,__VA_ARGS__)
inline static slice_t _buf_slice1(const buf_t b) {
  return (slice_t){ .p = b.p, .len = b.len };
}
inline static slice_t _buf_slice3(const buf_t b, usize start, usize len) {
  assert(start + len <= b.len);
  return (slice_t){ .p = (u8*)b.p + start, .len = b.len - len };
}

// Following functions returning bool returns false if buf_grow failed:

// buf_grow increases the capacity of b by at least extracap bytes
bool buf_grow(buf_t* b, usize extracap) WARN_UNUSED_RESULT;

// buf_reserve makes sure that there is at least minavail bytes available at b->v+b->len.
bool buf_reserve(buf_t* b, usize minavail);

// buf_alloc allocates len bytes at b->v+b->len and increments b->len by len.
// If there's not enough space available, buf_grow is called to grow b->cap.
// Returns a pointer to the beginning of the allocated range, or NULL if buf_grow failed.
u8* nullable buf_alloc(buf_t* b, usize len) WARN_UNUSED_RESULT;

// buf_push appends a byte
inline static bool buf_push(buf_t* b, u8 byte) {
  if (UNLIKELY(b->len >= b->cap) && UNLIKELY(!buf_grow(b, 1)))
    return false;
  b->bytes[b->len++] = byte;
  return true;
}

// buf_nullterm appends a 0 byte without increasing len
bool buf_nullterm(buf_t* b);

// buf_append appends len bytes to the end of the buffer by copying src
bool buf_append(buf_t* b, const void* src, usize len);

bool buf_appendrepr(buf_t* b, const void* src, usize len);

// buf_insert inserts bytes at index, shifting any existing data over
// e.g. buf_insert("abc", 1, "123") => "a123bc"
bool buf_insert(buf_t* b, usize index, const void* src, usize len);

// buf_fill appends len bytes (like buf_alloc + memset)
bool buf_fill(buf_t* b, u8 byte, usize len);

// buf_print appends a null-terminated string
inline static bool buf_print(buf_t* b, const char* cstr) {
  return buf_append(b, cstr, strlen(cstr));
}

// buf_printf appends a formatted string
bool buf_printf(buf_t* b, const char* fmt, ...) ATTR_FORMAT(printf, 2, 3);
bool buf_vprintf(buf_t* b, const char* fmt, va_list);

bool buf_print_u64(buf_t* b, u64 n, u32 base);
bool buf_print_u32(buf_t* b, u32 n, u32 base);
bool buf_print_leb128_u32(buf_t* b, u32 n);

ASSUME_NONNULL_END
