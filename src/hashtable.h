// SPDX-License-Identifier: Apache-2.0
#pragma once
ASSUME_NONNULL_BEGIN

typedef bool(*hashtable_eqfn_t)(const void* ent1, const void* ent2);
typedef usize(*hashtable_hashfn_t)(usize seed, const void* ent);

typedef struct {
  memalloc_t ma;
  usize      seed;    // hash seed passed to hashfn
  usize      cap;     // capacity
  usize      len;     // current number of entries stored in the table
  void*      entries;
} hashtable_t;

err_t hashtable_init(hashtable_t*, memalloc_t ma, usize entsize, usize lenhint);
void hashtable_dispose(hashtable_t*, usize entsize);

// hashtable_clear removes all entries from the hashtable in a very efficient manner
void hashtable_clear(hashtable_t*, usize entsize);

// hashtable_assign returns a pointer to the entry, or NULL if out of memory.
// If an entry equivalent to keyent is found, *added is set to false.
// Otherwise a new entry is inserted and keyent is copied to it, *added is true.
void* nullable hashtable_assign(
  hashtable_t*       ht,
  hashtable_hashfn_t hashfn,
  hashtable_eqfn_t   eqfn,
  usize              entsize,
  const void*        keyent,
  bool* nullable     added);

// hashtable_lookup returns a pointer to an entry equivalent to keyent,
// or NULL if not found.
void* nullable hashtable_lookup(
  const hashtable_t* ht,
  hashtable_hashfn_t hashfn,
  hashtable_eqfn_t   eqfn,
  usize              entsize,
  const void*        keyent);

// hashtable_del removes an entry equivalent to keyent. Returns fals if not found.
// Optimization detail: if keyent is a pointer to an entry in ht, for example from
// hashtable_lookup, no additional lookup is performed internally.
bool hashtable_del(
  hashtable_t*       ht,
  hashtable_hashfn_t hashfn,
  hashtable_eqfn_t   eqfn,
  usize              entsize,
  const void*        keyent);

//———————————————————————————————————————————————————————————————————————————————————————
// strset_t is a specialized hashtable that holds byte slices (slice_t.)
// byte arrays are managed (copied into ma) and null-terminated.
typedef struct { hashtable_t; } strset_t;

err_t strset_init(strset_t*, memalloc_t ma, usize lenhint);
void strset_dispose(strset_t*);
void strset_clear(strset_t*);

slice_t* nullable strset_assign(
  strset_t*, const void* key, usize keylen, bool* nullable added);
slice_t* nullable strset_lookup(const strset_t*, const void* key, usize keylen);
bool strset_del(strset_t*, const slice_t* key);

usize strset_hashfn(usize seed, const void* ent);
bool strset_eqfn(const void* ent1, const void* ent2);


ASSUME_NONNULL_END
