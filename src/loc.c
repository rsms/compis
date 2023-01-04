// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"


u32 locmap_inputid(locmap_t* lm, input_t* input, memalloc_t ma) {
  assertnotnull(input);
  for (u32 i = 1; i < lm->len; i++) {
    if (lm->v[i] == input)
      return i;
  }
  if (lm->len == 0) {
    if UNLIKELY(!array_reserve(input_t*, (array_t*)lm, ma, 8))
      return 0;
    lm->v[0] = NULL;
    lm->v[1] = input;
    lm->len = 2;
    return 1;
  }
  if UNLIKELY(!array_push(input_t*, (array_t*)lm, ma, input))
    return 0;
  return lm->len - 1;
}


loc_t loc_adjuststart(loc_t p, i32 deltacol) {
  i32 c = (i32)loc_col(p);
  i32 w = (i32)loc_width(p);
  deltacol = (deltacol > 0) ? MIN(deltacol, w) : MAX(deltacol, -c);
  return loc_make_unchecked(
    loc_inputid(p),
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
  return loc_make_unchecked(loc_inputid(a), loc_line(a), c, w);
}


usize loc_fmt(loc_t p, char* buf, usize bufcap, const locmap_t* lm) {
  input_t* input = loc_input(p, lm);
  if (input && loc_line(p) == 0)
    return snprintf(buf, bufcap, "%s", input->name);
  return snprintf(buf, bufcap, "%s:%u:%u",
    input ? input->name : "<input>", loc_line(p), loc_col(p));
}


origin_t _origin_make2(const locmap_t* m, loc_t loc) {
  return (origin_t){
    .input = loc_input(loc, m),
    .line = loc_line(loc),
    .column = loc_col(loc),
    .width = loc_width(loc),
  };
}

origin_t _origin_make3(const locmap_t* m, loc_t loc, u32 focus_col) {
  return (origin_t){
    .input = loc_input(loc, m),
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
  if (!a.input) {
    a.input = b.input;
  } else if (!b.input) {
    b.input = a.input;
  }

  if (a.line != b.line || a.input != b.input) {
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
