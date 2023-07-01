// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"


err_t locmap_init(locmap_t* lm) {
  memset(lm, 0, sizeof(lm->m));
  return rwmutex_init(&lm->mu);
}


void locmap_dispose(locmap_t* lm, memalloc_t ma) {
  assert(!rwmutex_isrlocked(&lm->mu));
  rwmutex_dispose(&lm->mu);
  array_dispose(srcfile_t*, (array_t*)&lm->m, ma);
}


void locmap_clear(locmap_t* lm) {
  rwmutex_lock(&lm->mu);
  lm->m.len = 0;
  rwmutex_unlock(&lm->mu);
}


u32 locmap_srcfileid(locmap_t* lm, srcfile_t* sf, memalloc_t ma) {
  assertnotnull(sf);
  rwmutex_lock(&lm->mu);

  u32 id = 1;
  assert(lm->m.len == 0 || lm->m.v[0] == NULL);

  for (; id < lm->m.len; id++) {
    if (lm->m.v[id] == sf)
      goto end;
  }

  if (lm->m.len == 0) {
    if UNLIKELY(!array_reserve(srcfile_t*, (array_t*)lm, ma, 8)) {
      dlog("out of memory");
      id = 0;
    } else {
      lm->m.v[0] = NULL;
      lm->m.v[1] = sf;
      lm->m.len = 2;
      id = 1;
    }
  } else if UNLIKELY(!array_push(srcfile_t*, (array_t*)lm, ma, sf)) {
    dlog("out of memory");
    id = 0;
  } else {
    id = lm->m.len - 1;
  }

end:
  rwmutex_unlock(&lm->mu);
  return id;
}


srcfile_t* nullable loc_srcfile(loc_t p, locmap_t* lm) {
  u32 id = loc_srcfileid(p);
  rwmutex_rlock(&lm->mu);
  srcfile_t* sf = lm->m.len > id ? lm->m.v[id] : NULL;
  rwmutex_runlock(&lm->mu);
  return sf;
}


loc_t loc_adjuststart(loc_t p, i32 deltacol) {
  i32 c = (i32)loc_col(p);
  i32 w = (i32)loc_width(p);
  deltacol = (deltacol > 0) ? MIN(deltacol, w) : MAX(deltacol, -c);
  return loc_make_unchecked(
    loc_srcfileid(p),
    loc_line(p),
    (u32)(c + deltacol),
    (u32)(w - deltacol)
  );
}


loc_t loc_union(loc_t a, loc_t b) {
  if (loc_line(a) != loc_line(b)) {
    // cross-line pos union not supported (can't be expressed with loc_t; use loc_tSpan instead)
    return a;
  }
  if (b == 0)
    return a;
  if (a == 0)
    return b;
  if (b < a) {
    loc_t tmp = a;
    a = b;
    b = tmp;
  }
  u32 c = loc_col(a);
  u32 w = loc_width(a);
  u32 aend = c + w;
  u32 bstart = loc_col(b);
  u32 bw = loc_width(b);
  if (bstart > aend)
    aend = bstart;
  w = aend - c + bw;
  return loc_make_unchecked(loc_srcfileid(a), loc_line(a), c, w);
}


usize loc_fmt(loc_t p, char* buf, usize bufcap, locmap_t* lm) {
  srcfile_t* sf = loc_srcfile(p, lm);
  if (sf && loc_line(p) == 0)
    return snprintf(buf, bufcap, "%s", sf->name.p);
  return snprintf(buf, bufcap, "%s:%u:%u",
    sf ? sf->name.p : "<input>", loc_line(p), loc_col(p));
}


origin_t _origin_make2(locmap_t* lm, loc_t loc) {
  return (origin_t){
    .file = loc_srcfile(loc, lm),
    .line = loc_line(loc),
    .column = loc_col(loc),
    .width = loc_width(loc),
  };
}

origin_t _origin_make3(locmap_t* lm, loc_t loc, u32 focus_col) {
  return (origin_t){
    .file = loc_srcfile(loc, lm),
    .line = loc_line(loc),
    .column = loc_col(loc),
    .width = loc_width(loc),
    .focus_col = focus_col,
  };
}


// static origin_t origin_min(origin_t a, origin_t b) {
//   return (a->line < b->line) ? a :
//          (b->line < a->line) ? b :
//          (a->column < b->column) ? a :
//          (b->column < a->column) ? b :
//          (a->width < b->width) ? a :
//          b;
// }


// static origin_t origin_max(origin_t a, origin_t b) {
//   return (a->line > b->line) ? a :
//          (b->line > a->line) ? b :
//          (a->column > b->column) ? a :
//          (b->column > a->column) ? b :
//          (a->width > b->width) ? a :
//          b;
// }


// static origin_t origin_max_extent(origin_t a, origin_t b) {
//   return (a->line > b->line) ? a :
//          (b->line > a->line) ? b :
//          (a->column + a->width > b->column + b->width) ? a :
//          b;
// }


origin_t origin_union(origin_t a, origin_t b) {
  if (!a.file) {
    a.file = b.file;
  } else if (!b.file) {
    b.file = a.file;
  }

  if (a.line != b.line || a.file != b.file) {
    // origin_t can't express spans across lines (for now)
    return a.line ? a : b;
  }

  u32 a_endcol = a.column + MAX(1, a.width);
  u32 b_endcol = b.column + MAX(1, b.width);

  a.column = MIN(a.column, b.column);
  a.width = MAX(a_endcol, b_endcol) - a.column;

  // note: leave a.focus_col unmodified

  return a;
}
