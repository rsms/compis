// hash table with open addressing (linear probing)
// SPDX-License-Identifier: Apache-2.0
//
// Entries can be of any byte size and are copied into the map.
// Deletion is very fast by simply marking an entry as "deleted",
// by setting two bits in a bitmap. This bitmap is allocated along with memory
// for the entries and sits at the end of the entries memory.
// The status of an entry is tracked using two bits in the bitmap.
// An entry is either Free (0b00),in Use (0b01) or Deleted (0b10).
// When probing for an entry not found at its ideal index, the status of an entry
// is queried: if it's "free" then a lookup fails and an insertion uses that slot.
// If the status is "deleted" a lookup keeps going as the entry might be found
// "further ahead." An assignment that comes across a "deleted" entry will use it
// to place a new entry.
//
// Here's an illustrated example of a table with five entries.
// There's one collision: d and e has the same hash.
//
//   index   0  1  2  3  4  5  6  7  8
//   status  U  U  F  U  F  F  F  U  U
//   value   a  b     c        d  e  f
//   hash    0  1     3        6  6  7
//
// Let's delete entry d: We simply mark its status as "Deleted":
//
//   index   0  1  2  3  4  5  6  7  8
//   status  U  U  F  U  F  F  D  U  U
//   value   a  b     c        d  e  f
//   hash    0  1     3        6  6  7
//                             ↑  ↑
//                            [1][2]
//
// Now if we look up entry e we will start by looking at entry at index 6 [1]
// and check if its status is "use" or not, which it wont be, so we move on to
// the next index, 7 [2] and do the same thing. This time its status is "use"
// and if the entry is considered equal, it is found. We stop our search short
// if we encounter an entry with a status of "Free" as we know there are no
// collisions after that point. Here's pseudo code of the general algorithm:
//
//   index = index_of(hash_of(entry_to_find))
//   while status_at(index % table_capacity) != STATUS_FREE:
//     if status_at(index % table_capacity) == STATUS_USE:
//       if entries_equal(entry_at(index), entry_to_find)
//         return entry_at(index)
//     index++
//   return NOT_FOUND
//
// The downside with this approach is that we rely on two things for performance:
//   1. very good hash distribution (there's no perfect hash function.)
//   2. lots of empty entries (to stop linear probe attempts for non-existing entries.)
// There's nothing we can do (in the hash table implementation) to mitigate issue 1,
// but we do make sure to track "load factor" and grow the table as soon as it reaches
// 55% occupancy. This factor is defined by LOAD_FACTOR & LOAD_FACTOR_MUL definitions.
//
#include "colib.h"
#include "hashtable.h"
#include "hash.h"


// bit_get2 gets the value of the two bits at index.
// index must be an even number.
inline static u8 bit_get2(const void* bits, usize index) {
  assert(IS_ALIGN2(index, 2));
  return (((const u8*)bits)[index / 8] >> (index % 8)) & (u8)3u;
}

// bit_set2 sets the two bits at bits[index:index+1] to value
inline static void bit_set2(const void* bits, usize index, u8 value) {
  assert(IS_ALIGN2(index, 2));
  assertf(value <= 3, "value 0x%x has more than 2 bits set", value);
  ((u8*)bits)[index / 8] |= (value << (index % 8));
}


// values of the two bits per entry in bitset
#define STATUS_FREE 0  // 0b00  entry is free and there are no subsequent collisions
#define STATUS_USE  1  // 0b01  entry is in use
#define STATUS_DEL  2  // 0b10  entry is deleted

// lf is a bit shift magnitude that does fast integer division
// i.e. cap-(cap>>lf) == (u32)((double)cap*0.75)
// i.e. grow when 1=50% 2=75% 3=88% 4=94% full
#define LOAD_FACTOR     1
#define LOAD_FACTOR_MUL 0.5 // with LOAD_FACTOR 1=0.5 2=0.25 3=0.125 4=0.0625


//#define HASHTABLE_TRACE
#ifdef HASHTABLE_TRACE
  #define trace(fmt, args...) _dlog(7, "HT", __FILE__, __LINE__, fmt, ##args)
#else
  #define trace(fmt, args...) ((void)0)
#endif


static usize idealcap(usize lenhint) {
  // lenhint + 1: must always have one free slot
  double f = ((double)(lenhint + 1ul) * LOAD_FACTOR_MUL) + 0.5;
  if UNLIKELY(f > (double)USIZE_MAX)
    return USIZE_MAX;
  return CEIL_POW2(lenhint + 1ul + (usize)f);
}


inline static usize bitset_size(usize cap) {
  // 2 bits per entry (4 entries per byte)
  return cap / 4;
}


static err_t alloc_entries(
  memalloc_t ma, usize* cap_inout, usize entsize, void** entries_out)
{
  usize nbyte;

  if (check_mul_overflow(*cap_inout, entsize, &nbyte))
    return ErrOverflow;

  usize bitset_nbyte = bitset_size(*cap_inout);
  if (check_add_overflow(nbyte, bitset_nbyte, &nbyte))
    return ErrOverflow;

  mem_t entmem = mem_alloc(ma, nbyte);
  if (!entmem.p)
    return ErrNoMem;

  // if allocator gave us more memory than we asked for,
  // adjust cap if we can maintain pow2
  usize actual_cap = (entmem.size - bitset_nbyte) / entsize;
  if (IS_POW2(actual_cap))
    *cap_inout = actual_cap;

  // zero bitset
  void* bitset = entmem.p + (*cap_inout)*entsize;
  memset(bitset, 0, bitset_nbyte);

  *entries_out = entmem.p;
  return 0;
}


err_t hashtable_init(hashtable_t* ht, memalloc_t ma, usize entsize, usize lenhint) {
  assert(lenhint > 0);
  usize cap = idealcap(lenhint);
  err_t err = alloc_entries(ma, &cap, entsize, &ht->entries);
  if (err)
    return err;

  ht->ma = ma;
  ht->seed = fastrand();
  ht->cap = cap;
  ht->len = 0;

  return 0;
}


void hashtable_dispose(hashtable_t* ht, usize entsize) {
  mem_freex(ht->ma, MEM(ht->entries, ht->cap * entsize));
}


void hashtable_clear(hashtable_t* ht, usize entsize) {
  ht->len = 0;
  void* bitset = ht->entries + ht->cap*entsize;
  memset(bitset, 0, bitset_size(ht->cap));
}


static bool hashtable_grow(
  hashtable_t* ht, hashtable_hashfn_t hashfn, hashtable_eqfn_t eqfn, usize entsize)
{
  usize newcap;
  if (check_mul_overflow(ht->cap, 2ul, &newcap))
    return false;

  trace("[grow] (cap %zu -> %zu, mem %zu -> %zu B)",
    ht->cap, newcap,
    bitset_size(ht->cap) + ht->cap*entsize,
    bitset_size(newcap) + newcap*entsize);

  void* newentries;
  err_t err = alloc_entries(ht->ma, &newcap, entsize, &newentries);
  if (err)
    return false;
  void* newbitset = newentries + newcap*entsize;
  void* oldbitset = ht->entries + ht->cap*entsize;

  // ht->seed = fastrand();

  // TODO: we could improve the efficiency here by scanning the bitset
  // for larger ranges of zero bits.
  // RSM does something very similar in bitset_find_unset_range.

  for (usize index = 0; index < ht->cap; index++) {
    if (bit_get2(oldbitset, index*2) != STATUS_USE)
      continue;
    void* oldent = ht->entries + index*entsize;
    usize index = hashfn(ht->seed, oldent) & (newcap - 1);
    while (bit_get2(newbitset, index*2)) {
      // assert that there are no duplicate entries
      assert(!eqfn(oldent, newentries + index*entsize));
      if (++index == newcap)
        index = 0;
    }
    void* newent = newentries + index*entsize;
    memcpy(newent, oldent, entsize);
    bit_set2(newbitset, index*2, STATUS_USE);
  }

  mem_freex(ht->ma, MEM(ht->entries, ht->cap * entsize));

  ht->entries = newentries;
  ht->cap = newcap;
  return true;
}


void* nullable hashtable_assign(
  hashtable_t*       ht,
  hashtable_hashfn_t hashfn,
  hashtable_eqfn_t   eqfn,
  usize              entsize,
  const void*        keyent,
  bool* nullable     added)
{
  usize growlen = ht->cap - (ht->cap >> LOAD_FACTOR);
  if (UNLIKELY(ht->len >= growlen) && !hashtable_grow(ht, hashfn, eqfn, entsize)) {
    if (added)
      *added = false;
    return NULL;
  }

  usize index = hashfn(ht->seed, keyent) & (ht->cap - 1);

  #ifdef HASHTABLE_TRACE
  char repr[128];
  { usize n = string_repr(repr, sizeof(repr), keyent, ht->entsize);
    trace("[assign] %.*s (hash=0x%lx, index=%zu)",
      (int)n, repr, hashfn(ht->seed, keyent), index); }
  #endif

  void* entries = ht->entries;
  void* bitset = entries + ht->cap*entsize;
  void* ent = entries + index*entsize;
  usize delidx = USIZE_MAX;

  for (u8 status; (status = bit_get2(bitset, index*2)); ) {

    #ifdef HASHTABLE_TRACE
    { usize n = string_repr(repr, sizeof(repr), ent, entsize);
      trace("  test %.*s (index=%zu)", (int)n, repr, index); }
    #endif

    if (status == STATUS_USE && eqfn(keyent, ent)) {
      // return existing equivalent entry
      trace("  ret existing (index=%zu)", index);
      if (added)
        *added = false;
      return ent;
    }

    #ifdef HASHTABLE_TRACE
    else if (status == STATUS_DEL) {
      trace("  skip deleted (index=%zu)", index);
      // Entry slot is marked as "deleted."
      // Insert here at index (unless we find an equivalent entry to return)
    }
    #endif

    if (++index == ht->cap) {
      index = 0;
      ent = entries;
    } else {
      ent += entsize;
    }
  }

  if (delidx != USIZE_MAX) {
    trace("  recycle deleted entry");
    index = delidx;
    ent = entries + index*entsize;
  }

  trace("  store new entry (index=%zu)", index);

  ht->len++;
  memcpy(ent, keyent, entsize);
  bit_set2(bitset, index*2, STATUS_USE);
  if (added)
    *added = true;
  return ent;
}


void* nullable hashtable_lookup(
  const hashtable_t*  ht,
  hashtable_hashfn_t  hashfn,
  hashtable_eqfn_t    eqfn,
  usize               entsize,
  const void*         keyent)
{
  usize index = hashfn(ht->seed, keyent) & (ht->cap - 1);
  void* entries = ht->entries;
  void* bitset = entries + ht->cap*entsize;
  void* ent;

  #ifdef HASHTABLE_TRACE
  {
    char repr[128];
    usize n = string_repr(repr, sizeof(repr), keyent, entsize);
    trace("[lookup] %.*s (hash=0x%lx, index=%zu)",
      (int)n, repr, hashfn(seed, keyent), index);
  }
  #endif

  for (u8 status; (status = bit_get2(bitset, index*2)); ) {
    if (status == STATUS_USE) {
      ent = entries + index*entsize;
      if (eqfn(keyent, ent)) {
        trace("  found (index=%zu)", index);
        return ent;
      }
    } else {
      trace("  skip deleted (index=%zu)", index);
      // status == STATUS_DEL
    }
    if (++index == ht->cap)
      index = 0;
    trace("  probe (index=%zu)", index);
  }

  trace("  not found");

  return NULL;
}


bool hashtable_del(
  hashtable_t*       ht,
  hashtable_hashfn_t hashfn,
  hashtable_eqfn_t   eqfn,
  usize              entsize,
  const void*        keyent)
{
  #ifdef HASHTABLE_TRACE
  {
    char repr[128];
    usize n = string_repr(repr, sizeof(repr), keyent, entsize);
    usize hash = hashfn(ht->seed, keyent);
    usize index = hash & (ht->cap - 1);
    trace("[del] %.*s (hash=0x%lx, index=%zu)", (int)n, repr, hash, index);
  }
  #endif

  if (ht->len == 1) {
    trace("  clear (len=1)");
    hashtable_clear(ht, entsize);
    return true;
  }

  void* ent;
  if (ht->entries <= keyent && keyent <= ht->entries + entsize*ht->cap) {
    // keyent is a pointer to an actual entry
    trace("  keyent is an ent; skip lookup");
    ent = (void*)keyent;
  } else {
    // find the actual entry which is equivalent to keyent
    ent = hashtable_lookup(ht, hashfn, eqfn, entsize, keyent);
    if (!ent)
      return false;
  }

  usize index = (ent - ht->entries) / entsize;
  void* bitset = ht->entries + ht->cap*entsize;

  // mark entry as deleted
  assertf(bit_get2(bitset, index*2) == STATUS_USE, "index %zu should be in use", index);
  bit_set2(bitset, index*2, STATUS_DEL);

  return true;
}


//———————————————————————————————————————————————————————————————————————————————————————
// strset_t

err_t strset_init(strset_t* ht, memalloc_t ma, usize lenhint) {
  return hashtable_init((hashtable_t*)ht, ma, sizeof(slice_t), lenhint);
}

void strset_dispose(strset_t* ht) {
  void* bitset = ht->entries + ht->cap*sizeof(slice_t);
  for (usize index = 0; index < ht->cap; index++) {
    if (bit_get2(bitset, index*2) == STATUS_USE) {
      slice_t s = ((slice_t*)ht->entries)[index];
      mem_freex(ht->ma, MEM((void*)s.p, s.len + 1));
    }
  }
  hashtable_dispose((hashtable_t*)ht, sizeof(slice_t));
}

void strset_clear(strset_t* ht) {
  hashtable_clear((hashtable_t*)ht, sizeof(slice_t));
}


usize strset_hashfn(usize seed, const void* ent) {
  static const u64 secret[4] = {
    0xdb1949b0945c5256llu,
    0x4f85e17c1e7ee8allu,
    0x24ac847a1c0d4bf7llu,
    0xd2952ed7e9fbaf43llu };
  const slice_t* s = ent;
  return wyhash(s->p, s->len, seed, secret);
}


bool strset_eqfn(const void* ent1, const void* ent2) {
  const slice_t* a = ent1;
  const slice_t* b = ent2;
  if (a->len != b->len)
    return false;
  return memcmp(a->p, b->p, a->len) == 0;
}


slice_t* nullable strset_assign(
  strset_t* ht, const void* key, usize keylen, bool* nullable added)
{
  bool added1;
  if (!added)
    added = &added1;

  slice_t* ent = hashtable_assign(
    (hashtable_t*)ht,
    strset_hashfn, strset_eqfn, sizeof(slice_t),
    &(slice_t){ .p = key, .len = keylen }, added);

  if (!ent || !*added)
    return ent;

  // hashtable_assign should have copied keyent to ent
  assert(ent->len == keylen);

  // allocate memory for ent's copy of key
  void* p = mem_alloc(ht->ma, ent->len + 1).p; // +1 for NUL terminator
  if UNLIKELY(!p) {
    // memory allocation failed; mark entry as "deleted" since it's invalid
    usize index = ((void*)ent - ht->entries) / sizeof(slice_t);
    void* bitset = ht->entries + ht->cap*sizeof(slice_t);
    bit_set2(bitset, index*2, STATUS_DEL);
    return NULL;
  }
  memcpy(p, key, ent->len);
  ((u8*)p)[ent->len] = 0; // NUL terminator
  ent->p = p;
  return ent;
}


slice_t* nullable strset_lookup(const strset_t* ht, const void* key, usize keylen) {
  slice_t keyent = { .p = key, .len = keylen };
  return hashtable_lookup(
    (hashtable_t*)ht, strset_hashfn, strset_eqfn, sizeof(slice_t), &keyent);
}


bool strset_del(strset_t* ht, const slice_t* keyent) {
  slice_t* ent;
  if (ht->entries <= (void*)keyent &&
      (void*)keyent <= ht->entries + sizeof(slice_t)*ht->cap)
  {
    // keyent is a pointer to an actual entry
    ent = (slice_t*)keyent;
  } else {
    // find the actual entry which is equivalent to keyent
    ent = hashtable_lookup(
      (hashtable_t*)ht, strset_hashfn, strset_eqfn, sizeof(slice_t), keyent);
    if (!ent)
      return false;
  }

  // free memory allocated for the string data at ent->p
  mem_freex(ht->ma, MEM((void*)ent->p, ent->len + 1));

  // mark entry as deleted
  usize index = ((void*)ent - ht->entries) / sizeof(slice_t);
  void* bitset = ht->entries + ht->cap*sizeof(slice_t);
  bit_set2(bitset, index*2, STATUS_DEL);

  return true;
}

/*
//———————————————————————————————————————————————————————————————————————————————————————
// ptrset_t is a specialized table that holds memory addresses (void*)
typedef struct { hashtable_t; } ptrset_t;

err_t ptrset_init(ptrset_t*, memalloc_t, usize lenhint);
void ptrset_dispose(ptrset_t*);
void ptrset_clear(ptrset_t*);

void* nullable ptrset_assign(ptrset_t*, const void* key, bool* nullable added);
void* nullable ptrset_lookup(const ptrset_t*, const void* key);
bool ptrset_del(ptrset_t* ht, const void* key);

usize ptrset_hashfn(usize seed, const void *ent);
bool ptrset_eqfn(const void* ent1, const void* ent2);

//———————————————————————————————————————————————————————————————————————————————————————
// ptrset_t

err_t ptrset_init(ptrset_t* ht, memalloc_t ma, usize lenhint) {
  return hashtable_init((hashtable_t*)ht, ma, sizeof(void*), lenhint);
}

void ptrset_dispose(ptrset_t* ht) {
  hashtable_dispose((hashtable_t*)ht, sizeof(void*));
}

void ptrset_clear(ptrset_t* ht) {
  hashtable_clear((hashtable_t*)ht, sizeof(void*));
}

usize ptrset_hashfn(usize seed, const void *ent) {
  return wyhash64((uintptr)*(void**)ent, seed);
}

bool ptrset_eqfn(const void* ent1, const void* ent2) {
  return *(void**)ent1 == *(void**)ent2;
}

void* nullable ptrset_assign(ptrset_t* ht, const void* key, bool* nullable added) {
  assertnotnull(key);
  void** ent = hashtable_assign(
    (hashtable_t*)ht, ptrset_hashfn, ptrset_eqfn, sizeof(void*), &key, added);
  if (ent == NULL)
    return NULL;
  return *ent;
}

void* nullable ptrset_lookup(const ptrset_t* ht, const void* key) {
  assertnotnull(key);
  void** ent = hashtable_lookup(
    (hashtable_t*)ht, ptrset_hashfn, ptrset_eqfn, sizeof(void*), key);
  if (ent == NULL)
    return NULL;
  assertnotnull(*ent);
  return *ent;
}

bool ptrset_del(ptrset_t* ht, const void* key) {
  assertnotnull(key);
  return hashtable_del(
    (hashtable_t*)ht, ptrset_hashfn, ptrset_eqfn, sizeof(void*), key);
}

*/

//———————————————————————————————————————————————————————————————————————————————————————
// tests
#ifdef CO_ENABLE_TESTS


typedef struct {
  const char s[6];
  usize      hash;
} testent_t;


static usize testent_hash(usize seed, const void* entp) {
  // return wyhash64((u64)*(u32*)ent, seed);

  // static const u64 secret[4] = {
  //   0xdb1949b0945c5256llu,
  //   0x4f85e17c1e7ee8allu,
  //   0x24ac847a1c0d4bf7llu,
  //   0xd2952ed7e9fbaf43llu };
  // const testent_t* ent = entp;
  // return wyhash(ent->s, strlen(ent->s), seed, secret);

  return ((testent_t*)entp)->hash;
}


static bool testent_eq(const void* a, const void* b) {
  return strcmp( ((testent_t*)a)->s, ((testent_t*)b)->s ) == 0;
}

UNITTEST_DEF(hashtable) {
  testent_t samples[] = {
    { "anne", 0x1 },
    { "bob",  0x2 },
    { "cat",  0x2 }, // hash collision with "bob"
    { "bob",  0x2 },
    { "ken",  0x3 }, // insert causes shift as "cat" will be stored at index 3
    { "sam",  0x5 }, // 5 to avoid another shift
  };
  testent_t samples2[] = {
    { "robin", 0x16 },
    { "mark",  0x17 },
    { "laila", 0x18 },
    { "fred",  0x19 },
    { "kara",  0x1a },
    { "fia",   0x1b },
    { "adam",  0x1c },
    { "mitch", 0x1d },
    { "wendy", 0x1e },
    { "pam",   0x1f },
  };
  usize expect_len = countof(samples) - 1; // -1 duplicate

  hashtable_t ht;
  testent_t* ent;
  bool added;
  bool ok;
  memalloc_t ma = memalloc_default();
  usize entsize = sizeof(testent_t);
  usize lenhint = countof(samples);
  assert(hashtable_init(&ht, ma, entsize, lenhint) == 0);

  // we need cap to be less than the total amount of sample input, so we can test growth
  assertf(ht.cap <= countof(samples) + countof(samples2),
    "%zu (can't test growth)", ht.cap);

  // dlog("ht.cap = %zu", ht.cap);
  // dlog("ht.len = %zu", ht.len);

  // hashtable_assign
  for (usize i = 0; i < countof(samples); i++) {
    ent = hashtable_assign(
      &ht, testent_hash, testent_eq, entsize, &samples[i], &added);
    assertf(ent, "out of memory");
    assert(strcmp(ent->s, samples[i].s) == 0);
  }
  assertf(ht.len == expect_len, "%zu == %zu", ht.len, expect_len);

  // hashtable_lookup
  for (usize i = 0; i < countof(samples); i++) {
    ent = hashtable_lookup(&ht, testent_hash, testent_eq, entsize, &samples[i]);
    assertf(ent, "'%s' not found", samples[i].s);
    assertf(strcmp(ent->s, samples[i].s) == 0,
      "found '%s' instead of '%s'", ent->s, samples[i].s);
  }

  // delete "sam" which should be straight forward as it's at its primary index
  ok = hashtable_del(&ht, testent_hash, testent_eq, entsize, &samples[5]);
  assertf(ok, "'%s'", samples[5].s);

  // "sam" should NOT be found
  ent = hashtable_lookup(&ht, testent_hash, testent_eq, entsize, &samples[5]);
  assertf(ent == NULL, "'%s'", samples[5].s);
  assertf(strcmp(samples[5].s, "sam") == 0, "'%s'", samples[5].s);

  // delete "bob" which leaves "cat" (same hash/index) in a subsequent position.
  // "cat" should be found by linear probing past STATUS_DEL after this.
  ok = hashtable_del(&ht, testent_hash, testent_eq, entsize, &samples[1]);
  assertf(ok, "'%s'", samples[1].s);

  // "bob" should NOT be found
  ent = hashtable_lookup(&ht, testent_hash, testent_eq, entsize, &samples[1]);
  assertf(ent == NULL, "'%s'", samples[1].s);
  assertf(strcmp(samples[1].s, "bob") == 0, "'%s'", samples[1].s);

  // "cat" SHOULD be found
  ent = hashtable_lookup(&ht, testent_hash, testent_eq, entsize, &samples[2]);
  assertf(ent != NULL, "'%s'", samples[2].s);
  assertf(strcmp(samples[2].s, "cat") == 0, "'%s'", samples[1].s);
  assertf(strcmp(ent->s, "cat") == 0, "got '%s'", ent->s);

  // assign more, causing growth
  for (usize i = 0; i < countof(samples2); i++) {
    ent = hashtable_assign(
      &ht, testent_hash, testent_eq, entsize, &samples2[i], &added);
    assertf(ent, "out of memory");
    assert(strcmp(ent->s, samples2[i].s) == 0);
    assert(added);
  }

  // hashtable_lookup
  for (usize i = 0; i < countof(samples2); i++) {
    ent = hashtable_lookup(&ht, testent_hash, testent_eq, entsize, &samples2[i]);
    assertf(ent, "'%s' not found", samples2[i].s);
    assertf(strcmp(ent->s, samples2[i].s) == 0,
      "found '%s' instead of '%s'", ent->s, samples2[i].s);
  }

  hashtable_dispose(&ht, entsize);
}


#endif // CO_ENABLE_TESTS
