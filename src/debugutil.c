// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "debugutil.h"


bool debug_histogram_fmt(buf_t* buf, usize* labels, usize* counts, usize len) {
  bool ok = true;

  int labelw = 0;
  usize maxcount = 0;
  for (usize i = 0; i < len; i++)
    labelw = MAX(labelw, (int)ndigits10(labels[i]));
  for (usize i = 0; i < len; i++)
    maxcount = MAX(maxcount, counts[i]);
  labelw = MAX(labelw, (int)ndigits10(maxcount));

  for (usize i = 0; i < len; i++)
    ok &= buf_printf(buf, &" %*zu"[!i], labelw, counts[i]);
  ok &= buf_push(buf, '\n');

  // TODO: scale maxcount; currently bars are very tall for large counts

  for (usize row = 0; row < maxcount; row++) {
    for (usize i = 0; i < len; i++) {
      usize count_for_row = maxcount - row;
      if (counts[i] < count_for_row) {
        ok &= buf_printf(buf, &" %*s"[!i], labelw, "");
      } else if (labelw > 1) {
        ok &= buf_printf(buf, &" %*s██"[!i], MAX(0, labelw - 2), "");
      } else {
        ok &= buf_printf(buf, &" %*s█"[!i], MAX(0, labelw - 1), "");
      }
    }
    ok &= buf_push(buf, '\n');
  }

  for (usize i = 0; i < len; i++)
    ok &= buf_printf(buf, &" %*zu"[!i], labelw, labels[i]);
  ok &= buf_push(buf, '\n');

  return ok;
}
