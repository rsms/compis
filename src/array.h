// growable typed array
// SPDX-License-Identifier: Apache-2.0
//
// Example
//   array_t a;
//   array_init(&a);
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
  u32 len, cap; // count of T items (not bytes)
#ifdef DEBUG
  memalloc_t ma;
#endif
} array_t;

typedef int(*array_sorted_cmp_t)(const void* aptr, const void* bptr, void* ctx);

#if __documentation__

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
bool array_reserve_exact(T, array_t*, memalloc_t, u32 minavail);
bool array_shrinkwrap(T, array_t* a, memalloc_t ma); // reduce memory, set cap=len
void array_remove(T, array_t*, u32 start, u32 len);
slice_t array_slice(const array_t a);
slice_t array_slice(const array_t a, usize start, usize len);

// array_sortedset_assign keeps the array sorted while inserting only unique elements.
// If elements are added to the array in some other way, the array may not be sorted,
// so make sure to add all elements using this function (or don't use it at all.)
// If there's an equivalent element in the array, its pointer is returned instead
// of allocating a new slot.
T* nullable array_sortedset_assign(
  T, array_t* a, memalloc_t, const T* vptr,
  array_sorted_cmp_t cmpf, void* nullable ctx);

// array_sortedset_lookup works like array_sortedset_assign but just looks a value up,
// it does not add it if its not found. If a value is not found, NULL is returned.
// When a value is found, *indexp is set to its index.
T* nullable array_sortedset_lookup(
  T, const array_t* a, const T* vptr, u32* indexp,
  array_sorted_cmp_t cmpf, void* nullable ctx);

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

#endif//__documentation__ —————————————————————————————————————————————————————————————
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
#define array_sortedset_assign(T, a, ma, valptr, cmpf, ctx) \
  (T*)_array_sortedset_assign((array_t*)(a), (ma), sizeof(T), (valptr), (cmpf), (ctx))
#define array_sortedset_lookup(T, a, valptr, indexp, cmpf, ctx) \
  (T*)_array_sortedset_lookup(\
    (const array_t*)(a), sizeof(T), (valptr), (indexp), (cmpf), (ctx))
#define array_push(T, a, ma, val) ({ \
  static_assert(co_same_type(T, __typeof__(val)), ""); \
  array_t* __a = (a); \
  ( __a->len >= __a->cap && UNLIKELY(!_array_grow(__a, (ma), sizeof(T), 1)) ) ? false : \
    ( ( (T*)__a->ptr )[__a->len++] = (val), true ); \
})
#define array_pop(T, a) ( ((T*)(a)->ptr)[--a->len] )
#define array_reserve(T, a, ma, minavail) \
  _array_reserve((a), (ma), sizeof(T), (minavail))
#define array_reserve_exact(T, a, ma, minavail) \
  _array_reserve_exact((a), (ma), sizeof(T), (minavail))
#define array_shrinkwrap(T, a, ma)     _array_shrinkwrap((a), (ma), sizeof(T));
#define array_remove(T, a, start, len) _array_remove((a), sizeof(T), (start), (len))
#define array_move(T, a, dst, start, end) \
  _ARRAY_MOVE(sizeof(T), (void*)(a)->ptr, (usize)(dst), (usize)(start), (usize)(end))


// array_slice returns a slice of memory
// 1. array_slice<T >= array_t>(const T a)
// 2. array_slice<T >= array_t>(const T a, usize start, usize len)
#define array_slice(...) __VARG_DISP(_array_slice,__VA_ARGS__)
#define _array_slice1(a) \
  ((slice_t){ .p = ((array_t*)&(a))->ptr, .len = ((array_t*)&(a))->len })
#define _array_slice3(a, start_, len_) ({ \
  u32 start__ = (start_), len__ = (len_); \
  safecheck(start__ + len__ <= ((array_t*)&(a))->len); \
  (slice_t){ .p = ((array_t*)&(a))->ptr + start__, \
             .len = ((array_t*)&(a))->len - len__ }; \
})


bool _array_grow(array_t* a, memalloc_t ma, u32 elemsize, u32 extracap);
bool _array_shrinkwrap(array_t* a, memalloc_t ma, usize elemsize);
void _array_dispose(array_t* a, memalloc_t ma, u32 elemsize);
void _array_remove(array_t* a, u32 elemsize, u32 start, u32 len);
void* nullable _array_alloc(array_t* a, memalloc_t ma, u32 elemsize, u32 len);
void* nullable _array_allocat(array_t* a, memalloc_t ma, u32 elemsize, u32 i, u32 len);
bool _array_reserve(array_t* a, memalloc_t ma, u32 elemsize, u32 minavail);
bool _array_reserve_exact(array_t* a, memalloc_t ma, u32 elemsize, u32 minavail);
void* nullable _array_sortedset_assign(
  array_t* a, memalloc_t ma, u32 elemsize,
  const void* valptr, array_sorted_cmp_t cmpf, void* nullable ctx);
void* nullable _array_sortedset_lookup(
  const array_t* a, u32 elemsize,
  const void* valptr, u32* indexp, array_sorted_cmp_t cmpf, void* nullable ctx);

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
    u32array_init(&a);

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
static bool        NAME_reserve_exact(NAME_t* a, memalloc_t ma, u32 minavail)
static bool        NAME_shrinkwrap(NAME_t* a, memalloc_t ma)
static void        NAME_remove(NAME_t* a, u32 start, u32 len)
static void        NAME_move(NAME_t* a, u32 dst, u32 start, u32 end)
#endif//__documentation__

// array_type(NAME T)
#if DEBUG
  #define array_type(T) struct { \
    T* nullable v; \
    u32 len, cap; \
    memalloc_t ma; \
  }
#else
  #define array_type(T) struct { \
    T* nullable v; \
    u32 len, cap; \
  }
#endif

#define DEF_ARRAY_TYPE_API(T, NAME) \
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
  UNUSED inline static bool NAME##_reserve_exact(\
    NAME##_t* a, memalloc_t ma, u32 minavail)\
  {\
    return array_reserve_exact(T, (array_t*)(a), ma, minavail); } \
  UNUSED inline static bool NAME##_shrinkwrap(NAME##_t* a, memalloc_t ma){\
    return array_shrinkwrap(T, (array_t*)(a), ma); } \
  UNUSED inline static void NAME##_remove(NAME##_t* a, u32 start, u32 len) { \
    array_remove(T, (array_t*)(a), start, len); } \
  UNUSED inline static void NAME##_move(NAME##_t* a, u32 dst, u32 start, u32 end){\
    array_move(T, (array_t*)(a), dst, start, end); }

#define DEF_ARRAY_TYPE_NULLABLEPTR_API(T, NAME) \
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
  UNUSED inline static bool NAME##_reserve_exact( \
    NAME##_t* a, memalloc_t ma, u32 minavail) \
  { \
    return array_reserve_exact(T, (array_t*)(a), ma, minavail); } \
  UNUSED inline static bool NAME##_shrinkwrap(NAME##_t* a, memalloc_t ma){\
    return array_shrinkwrap(T, (array_t*)(a), ma); } \
  UNUSED inline static void NAME##_remove(NAME##_t* a, u32 start, u32 len) { \
    array_remove(T, (array_t*)(a), start, len); } \
  UNUSED inline static void NAME##_move(NAME##_t* a, u32 dst, u32 start, u32 end){\
    array_move(T, (array_t*)(a), dst, start, end); }


typedef array_type(u32) u32array_t;
DEF_ARRAY_TYPE_API(u32, u32array)

typedef array_type(void*) ptrarray_t;

DEF_ARRAY_TYPE_NULLABLEPTR_API(void*, ptrarray)
u32 ptrarray_rindexof(const ptrarray_t* a, const void* value); // U32_MAX if not found
u32 u32array_rindexof(const u32array_t* a, u32 value); // U32_MAX if not found

// ptrarray_move_to_end is equivalent to ptrarray_move(a, a->len-1, index, index+1)
void ptrarray_move_to_end(ptrarray_t* a, u32 index);

// returns false if memory allocation failed
bool ptrarray_sortedset_addcstr(ptrarray_t* a, memalloc_t ma, const char* str);
bool ptrarray_sortedset_addptr(
  ptrarray_t* a, memalloc_t ma, const void* ptr, bool* nullable added_out);
bool u32array_sortedset_add(u32array_t* a, memalloc_t ma, u32 v);

ASSUME_NONNULL_END
