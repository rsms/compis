// SPDX-License-Identifier: Apache-2.0
#include "c0lib.h"
#include "compiler.h"


// void posmap_init(posmap_t* pm) {
//   // the first slot is used to return NULL in pos_source for unknown positions
//   pm->a.v[0] = NULL;
//   pm->a.len++;
// }


u32 posmap_origin(posmap_t* pm, input_t* input, memalloc_t ma) {
  assertnotnull(input);
  for (u32 i = 1; i < pm->len; i++) {
    if (pm->v[i] == input)
      return i;
  }
  if (pm->len == 0) {
    if UNLIKELY(!array_reserve(input_t*, (array_t*)pm, ma, 8))
      return 0;
    pm->v[0] = NULL;
    pm->v[1] = input;
    pm->len = 2;
    return 1;
  }
  if UNLIKELY(!array_push(input_t*, (array_t*)pm, ma, input))
    return 0;
  return pm->len - 1;
}


pos_t pos_adjuststart(pos_t p, i32 deltacol) {
  i32 c = (i32)pos_col(p);
  i32 w = (i32)pos_width(p);
  deltacol = (deltacol > 0) ? MIN(deltacol, w) : MAX(deltacol, -c);
  return pos_make_unchecked(
    pos_origin(p),
    pos_line(p),
    (u32)(c + deltacol),
    (u32)(w - deltacol)
  );
}


pos_t pos_union(pos_t a, pos_t b) {
  if (pos_line(a) != pos_line(b)) {
    // cross-line pos union not supported (can't be expressed with pos_t; use pos_tSpan instead)
    return a;
  }
  if (b == 0)
    return a;
  if (a == 0)
    return b;
  if (b < a) {
    pos_t tmp = a;
    a = b;
    b = tmp;
  }
  u32 c = pos_col(a);
  u32 w = pos_width(a);
  u32 aend = c + w;
  u32 bstart = pos_col(b);
  u32 bw = pos_width(b);
  if (bstart > aend)
    aend = bstart;
  w = aend - c + bw;
  return pos_make_unchecked(pos_origin(a), pos_line(a), c, w);
}


usize pos_fmt(pos_t p, char* buf, usize bufcap, const posmap_t* pm) {
  input_t* input = pos_input(p, pm);
  return snprintf(buf, bufcap, "%s:%u:%u",
    input ? input->name : "<input>", pos_line(p), pos_col(p));
}
