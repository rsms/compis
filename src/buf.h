// growable byte buffer (specialization of array)
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

inline static usize buf_avail(const buf_t* b) { return b->cap - b->len; }

// buf_grow increases the capacity of b by at least extracap bytes.
// If grow fails, false is returned and b remains unchanged.
bool buf_grow(buf_t* b, usize extracap) WARN_UNUSED_RESULT;

// buf_reserve makes sure that there is at least minavail bytes available at b->v+b->len.
bool buf_reserve(buf_t* b, usize minavail);

// buf_alloc allocates len bytes at b->v+b->len and increments b->len by len.
// If there's not enough space available, buf_grow is called to grow b->cap.
// Returns a pointer to the beginning of the allocated range, or NULL if buf_grow failed.
u8* nullable buf_alloc(buf_t* b, usize len) WARN_UNUSED_RESULT;

// buf_push appends a byte to the end of the buffer.
// Returns false if buf_grow was called but failed.
bool buf_push(buf_t* b, u8 byte) WARN_UNUSED_RESULT;

// buf_append appends len bytes to the end of the buffer by copying src.
// Returns false if buf_grow was called but failed.
bool buf_append(buf_t* b, const void* src, usize len) WARN_UNUSED_RESULT;

ASSUME_NONNULL_END
