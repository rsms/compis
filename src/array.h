// growable typed array
// SPDX-License-Identifier: Apache-2.0
//
// Example
//   array_t a;
//   array_init(&a, ma);
//   for (u32 i = 0; i < 1024; i++)
//     assert( array_push(u32, &a, i) );
//   for (u32 i = 0; i < 1024; i++)
//     assert( array_at_safe(u32, &a, i) == i );
//   array_dispose(u32, &a);
//
#pragma once
ASSUME_NONNULL_BEGIN

typedef struct {
  u8* nullable ptr;
  u32 cap, len; // count of T items (not bytes)
#if DEBUG
  memalloc_t ma;
#endif
} array_t;

#if C0_API_DOC

// array_make returns an initialized empty array
static array_t array_make();
void array_init(array_t*);
void array_dispose(T, array_t*, memalloc_t);
T array_at(T, array_t*, u32 index);
T array_at_safe(T, array_t*, u32 index);
T* array_ptr(T, array_t*, u32 index);
T* array_ptr_safe(T, array_t*, u32 index);
T* nullable array_alloc(T, array_t*, memalloc_t, u32 len);
T* nullable array_allocat(T, array_t*, memalloc_t, u32 index, u32 len);
bool array_push(T, array_t*, memalloc_t, T value);
T array_pop(T, array_t*);
bool array_reserve(T, array_t*, memalloc_t, u32 minavail);
void array_remove(T, array_t*, u32 start, u32 len);

// array_move moves the chunk [start,end) to index dst. For example:
//
//   array_move(T, a, 5, 1, 3) : [ 0│1 2│3 4│5 6 7 ] ⟹ [ 0 3 4 1 2 5 6 7 ]
//                                  │←—→│  →│
//                              start  end  dst
//
//   array_move(T, a, 1, 4, 8) : [ 0│1 2 3│4 5 6 7│] ⟹ [ 0 4 5 6 7 1 2 3 ]
//                                  │←    │←—————→│
//                                 dst  start    end
//
void array_move(T, array_t*, u32 dst, u32 start, u32 end);

#endif // ————————————————————————————————————————————————————————————————————————————
// implementation

void array_init(array_t* a);
inline static array_t array_make() { return (array_t){ 0 }; }

#define array_dispose(T, a, ma)     _array_dispose((a), (ma), sizeof(T))
#define array_at(T, a, i)           ( ((T*)(a)->ptr)[i] )
#define array_at_safe(T, a, i)      ( safecheck((i)<(a)->len), array_at(T,(a),(i)) )
#define array_ptr(T, a, i)          ( (T*)(a)->ptr + (i) )
#define array_ptr_safe(T, a, i)     ( safecheck((i)<(a)->len), array_ptr(T,(a),(i)) )
#define array_alloc(T, a, ma, len)  ( (T*)_array_alloc((a), (ma), sizeof(T), (len)) )
#define array_allocat(T, a, ma, i, len) \
  ( (T*)_array_allocat((a), (ma), sizeof(T), (i), (len)) )
#define array_push(T, a, ma, val) ({ \
  static_assert(c0_same_type(T, __typeof__(val)), ""); \
  array_t* __a = (a); \
  ( __a->len >= __a->cap && UNLIKELY(!_array_grow(__a, (ma), sizeof(T), 1)) ) ? false : \
    ( ( (T*)__a->ptr )[__a->len++] = (val), true ); \
})
#define array_pop(T, a) ( ((T*)(a)->ptr)[--a->len] )
#define array_reserve(T, a, ma, minavail) \
  _array_reserve((a), (ma), sizeof(T), (minavail))
#define array_remove(T, a, start, len) \
  _array_remove((a), sizeof(T), (start), (len))
#define array_move(T, a, dst, start, end) \
  _ARRAY_MOVE(sizeof(T), (void*)(a)->ptr, (usize)(dst), (usize)(start), (usize)(end))


bool _array_grow(array_t* a, memalloc_t ma, u32 elemsize, u32 extracap);
void _array_dispose(array_t* a, memalloc_t ma, u32 elemsize);
void _array_remove(array_t* a, u32 elemsize, u32 start, u32 len);
void* nullable _array_alloc(array_t* a, memalloc_t ma, u32 elemsize, u32 len);
void* nullable _array_allocat(array_t* a, memalloc_t ma, u32 elemsize, u32 i, u32 len);
bool _array_reserve(array_t* a, memalloc_t ma, u32 elemsize, u32 minavail);

// void _ARRAY_MOVE(usize elemsize, void* v, usize dst, usize start, usize end)
#define _ARRAY_MOVE(elemsize, v, dst, start, end) (                               \
  (elemsize) == 4 ? _ARRAY_MOVE1(_arotate32,(dst),(start),(end),(u32* const)(v)) : \
  (elemsize) == 8 ? _ARRAY_MOVE1(_arotate64,(dst),(start),(end),(u64* const)(v)) : \
                    _ARRAY_MOVE1(_arotatemem,(dst),(start),(end),(elemsize),(v)) )
#define _ARRAY_MOVE1(f, dst, start, end, args...) (     \
  ((start)==(dst)||(start)==(end)) ? ((void)0) :         \
  ((start) > (dst)) ? (f)(args, (dst), (start), (end)) : \
                      (f)(args, (start), (end), (dst)+1lu) )

// AROTATE rotates the order of v in the range [first,last) in such a way
// that the element pointed to by "mid" becomes the new "first" element.
// Assumes first <= mid < last.
//
// void AROTATE(usize elemsize, void* array, usize first, usize mid, usize last)
#define AROTATE(elemsize, v, first, mid, last) (                          \
  (elemsize) == 4 ? _arotate32((u32* const)(v), (first), (mid), (last)) : \
  (elemsize) == 8 ? _arotate64((u64* const)(v), (first), (mid), (last)) : \
  _arotatemem((elemsize), (v), (first), (mid), (last)) )
void _arotatemem(usize stride, void* v, usize first, usize mid, usize last);
void _arotate32(u32* const v, usize first, usize mid, usize last);
void _arotate64(u64* const v, usize first, usize mid, usize last);

/*
DEF_ARRAY_TYPE defines an array type and inline functions.

E.g.
  DEF_ARRAY_TYPE(u32, u32array)
  void example(memalloc_t ma) {
    u32array_t a;
    u32array_init(&a, ma);

    // no type argument needed
    for (u32 i = 0; i < 1024; i++)
      u32array_push(&a, i);

    // direct array access
    for (u32 i = 0; i < 1024; i++)
      assert(a.v[i] == i)

    u32array_dispose(&a);
  }
*/
#ifdef __documentation__
typedef struct { T* nullable v; u32 cap, len; } NAME_t
static void        NAME_init(NAME_t* a)
static void        NAME_dispose(NAME_t* a, memalloc_t ma)
static void        NAME_clear(NAME_t* a)
static T           NAME_at_safe(NAME_t* a, u32 i)
static T*          NAME_ptr_safe(NAME_t* a, u32 i)
static bool        NAME_push(NAME_t* a, memalloc_t ma, T val)
static T           NAME_pop(NAME_t* a)
static bool        NAME_insert(NAME_t* a, memalloc_t ma, u32 at_index, T val)
static T* nullable NAME_alloc(NAME_t* a, memalloc_t ma, u32 len)
static T* nullable NAME_allocat(NAME_t*, memalloc_t ma, u32 index, u32 len);
static bool        NAME_reserve(NAME_t* a, memalloc_t ma, u32 minavail)
static void        NAME_remove(NAME_t* a, u32 start, u32 len)
static void        NAME_move(NAME_t* a, u32 dst, u32 start, u32 end)
#endif//__documentation__

#define DEF_ARRAY_TYPE(T, NAME) \
  typedef struct {              \
    union {                     \
      struct {                  \
        T* nullable v;          \
      };                        \
      array_t;                  \
    };                          \
  } NAME##_t;                   \
  \
  UNUSED inline static void NAME##_init(NAME##_t* a) { \
    array_init((array_t*)(a)); } \
  UNUSED inline static void NAME##_dispose(NAME##_t* a, memalloc_t ma) { \
    array_dispose(T, (array_t*)(a), ma); } \
  UNUSED inline static void NAME##_clear(NAME##_t* a) { \
    a->len = 0; } \
  UNUSED inline static T NAME##_at_safe(NAME##_t* a, u32 i) { \
    return array_at_safe(T, (array_t*)(a), i); } \
  UNUSED inline static T* NAME##_ptr_safe(NAME##_t* a, u32 i) { \
    return array_ptr_safe(T, (array_t*)(a), i); } \
  UNUSED inline static bool NAME##_push(NAME##_t* a, memalloc_t ma, T val) { \
    return array_push(T, (array_t*)(a), ma, val); } \
  UNUSED inline static T NAME##_pop(NAME##_t* a) { \
    return a->v[--a->len]; } \
  UNUSED inline static T* nullable NAME##_alloc(NAME##_t* a, memalloc_t ma, u32 len){\
    return array_alloc(T, (array_t*)(a), ma, len); } \
  UNUSED inline static T* nullable NAME##_allocat(\
    NAME##_t* a, memalloc_t ma, u32 i, u32 len) {\
      return array_allocat(T, (array_t*)(a), ma, i, len); } \
  UNUSED inline static bool NAME##_insert( \
    NAME##_t* a, memalloc_t ma, u32 at_index, T val) { \
      T* vp = NAME##_allocat(a, ma, at_index, 1); \
      return LIKELY(vp) ? (*vp = val, true) : false; } \
  UNUSED inline static bool NAME##_reserve(NAME##_t* a, memalloc_t ma, u32 minavail){\
    return array_reserve(T, (array_t*)(a), ma, minavail); } \
  UNUSED inline static void NAME##_remove(NAME##_t* a, u32 start, u32 len) { \
    array_remove(T, (array_t*)(a), start, len); } \
  UNUSED inline static void NAME##_move(NAME##_t* a, u32 dst, u32 start, u32 end){\
    array_move(T, (array_t*)(a), dst, start, end); }

#define DEF_ARRAY_TYPE_NULLABLEPTR(T, NAME) \
  typedef struct {              \
    union {                     \
      struct {                  \
        T* nullable v;          \
      };                        \
      array_t;                  \
    };                          \
  } NAME##_t;                   \
  \
  UNUSED inline static void NAME##_init(NAME##_t* a) { \
    array_init((array_t*)(a)); } \
  UNUSED inline static void NAME##_dispose(NAME##_t* a, memalloc_t ma) { \
    array_dispose(T, (array_t*)(a), ma); } \
  UNUSED inline static void NAME##_clear(NAME##_t* a) { \
    a->len = 0; } \
  UNUSED inline static T nullable NAME##_at_safe(NAME##_t* a, u32 i) { \
    return array_at_safe(T, (array_t*)(a), i); } \
  UNUSED inline static T* NAME##_ptr_safe(NAME##_t* a, u32 i) { \
    return array_ptr_safe(T, (array_t*)(a), i); } \
  UNUSED inline static bool NAME##_push(NAME##_t* a, memalloc_t ma, T nullable val) { \
    return array_push(T, (array_t*)(a), ma, val); } \
  UNUSED inline static T nullable NAME##_pop(NAME##_t* a) { \
    return a->v[--a->len]; } \
  UNUSED inline static T* nullable NAME##_alloc(NAME##_t* a, memalloc_t ma, u32 len){\
    return array_alloc(T, (array_t*)(a), ma, len); } \
  UNUSED inline static T* nullable NAME##_allocat(\
    NAME##_t* a, memalloc_t ma, u32 at_index, u32 len) {\
      return array_allocat(T, (array_t*)(a), ma, at_index, len); } \
  UNUSED inline static bool NAME##_insert( \
    NAME##_t* a, memalloc_t ma, u32 at_index, T nullable val) { \
      T* vp = NAME##_allocat(a, ma, at_index, 1); \
      return LIKELY(vp) ? (*vp = val, true) : false; } \
  UNUSED inline static bool NAME##_reserve(NAME##_t* a, memalloc_t ma, u32 minavail){\
    return array_reserve(T, (array_t*)(a), ma, minavail); } \
  UNUSED inline static void NAME##_remove(NAME##_t* a, u32 start, u32 len) { \
    array_remove(T, (array_t*)(a), start, len); } \
  UNUSED inline static void NAME##_move(NAME##_t* a, u32 dst, u32 start, u32 end){\
    array_move(T, (array_t*)(a), dst, start, end); }


DEF_ARRAY_TYPE_NULLABLEPTR(void*, ptrarray)

ASSUME_NONNULL_END
