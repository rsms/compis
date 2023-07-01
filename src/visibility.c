// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"


const char* visibility_str(nodeflag_t flags) {
  switch (flags & NF_VIS_MASK) {
    case NF_VIS_UNIT: return "private";
    case NF_VIS_PKG:  return "pkg";
    case NF_VIS_PUB:  goto pub;
  }
  assertf(0, "invalid visibility value %x (corrupt node flags %x)",
    flags & NF_VIS_MASK, flags);
pub:
  return "pub";
}
