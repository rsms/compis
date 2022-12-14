// hash map
// SPDX-License-Identifier: Apache-2.0
ASSUME_NONNULL_BEGIN

typedef struct {
  const void* nullable key; // NULL if this entry is empty
  usize                keysize;
  void* nullable       value;
} mapent_t;

typedef struct map {
  u32       cap, len; // capacity of entries, current number of items in map
  usize     seed;     // hash seed
  mapent_t* nullable entries; // not null in practice, just to satisfy msan
  const struct map* nullable parent;
} map_t;

bool map_init(map_t* m, memalloc_t ma, u32 lenhint); // false if mem_alloc fails
inline static void map_dispose(map_t* m, memalloc_t ma) {
  mem_freetv(ma, m->entries, m->cap);
}
void map_clear(map_t* m); // remove all items (m remains valid)

// map_reserve makes sure there is space for at least additional_space without
// rehashing on assignment.
bool map_reserve(map_t* m, memalloc_t ma, u32 additional_space);

mapent_t* nullable map_assign_ent(
  map_t* m, memalloc_t ma, const void* key, usize keysize);

void** nullable map_assign(map_t* m, memalloc_t ma, const void* key, usize keysize);
void** nullable map_lookup(const map_t* m, const void* key, usize keysize);
bool map_del(map_t* m, const void* key, usize keysize);

void** nullable map_assign_ptr(map_t* m, memalloc_t ma, const void* key);
void** nullable map_lookup_ptr(const map_t* m, const void* key);
bool map_del_ptr(map_t* m, const void* key);

// Iterator. Example with c-string keys
// for (const mapent_t* e = map_it(m); map_itnext(m, &e); )
//   dlog("%*.s => %zx", (int)e->keysize, (const char*)e->key, e->value);
inline static const mapent_t* nullable map_it(const map_t* m) { return m->entries-1; }
bool map_itnext(const map_t* m, const mapent_t** ep);

// MAP_STORAGE_X calculates the number of bytes needed to store len entries
#define MAP_STORAGE_X(len) \
  ( CEIL_POW2_X( ( ((usize)(len)) +1 ) * 2) * sizeof(mapent_t) )

ASSUME_NONNULL_END
