#include "colib.h"
#include "bgtask.h"

#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>

#define LINEBUF_CAP   512lu
#define DONE_PREFIX   "done in "
#define FANCY_START   "\x1B[1F"
#define FANCY_END     "\x1B[K\n"
#define OUTPUT_FILE   stdout
#define OUTPUT_FILENO STDOUT_FILENO

// BUFAVAIL: given a pointer 'ptr' into bgt->linebuf, return available space at ptr
#define BUFAVAIL(bgt, ptr)  ((usize)(uintptr)(((bgt)->linebuf + LINEBUF_CAP) - (ptr)))


bgtask_t* bgtask_open(memalloc_t ma, const char* name, u32 ntotal, int flags) {
  bgtask_t* bgt = mem_alloc(ma, sizeof(bgtask_t) + LINEBUF_CAP).p;
  if (!bgt)
    panic("out of memory");

  memset(bgt, 0, sizeof(bgtask_t));
  bgt->ma = ma;
  bgt->ntotal = ntotal;
  bgt->start_time = nanotime();
  bgt->fpos = -1000000;

  // set BGTASK_FANCY (unless set in flags)
  if ((flags & (BGTASK_NOFANCY | BGTASK_FANCY)) == 0) {
    const char* term = getenv("TERM");
    if (isatty(OUTPUT_FILENO) && term && strcmp(term, "dumb") != 0)
      flags |= BGTASK_FANCY;
  }
  bgt->flags = flags;

  // build prefix
  char* buf = bgt->linebuf;
  if (bgt->flags & BGTASK_FANCY)
    buf += strlen(FANCY_START); // reserve space
  *buf++ = '[';
  usize namelen = MIN((usize)LINEBUF_CAP/2, strlen(name));
  if (namelen > 0) {
    memcpy(buf, name, namelen);
    buf += namelen;
  }
  bgt->prefixlen = (u32)(uintptr)(buf - bgt->linebuf);

  // print initial status
  if (bgt->flags & BGTASK_FANCY)
    bgtask_setstatus(bgt, name);

  return bgt;
}


void bgtask_close(bgtask_t* bgt) {
  memalloc_t ma = assertnotnull(bgt->ma);
  usize size = sizeof(bgtask_t) + LINEBUF_CAP;
  #if CO_SAFE
    memset(bgt, 0, size);
  #endif
  mem_freex(ma, MEM(bgt, size));
}


static char* _clip_ellipsis(bgtask_t* bgt, usize len, usize maxlen) {
  char* buf;
  assert(len > maxlen);

  if (maxlen < 4) {
    buf = bgt->linebuf;
    if (maxlen > 2) *buf++ = '.';
    if (maxlen > 1) *buf++ = '.';
    if (maxlen > 0) *buf++ = '.';
    return buf;
  }

  usize nnonprint = 1; // leading '\r'
  usize left = (maxlen - 3/*"..."*/) / 2;
  usize right = (maxlen - 3/*"..."*/) - left;
  buf = &bgt->linebuf[left + nnonprint];
  memcpy(buf, "...", 3);
  buf += 3;
  len += nnonprint;
  memmove(buf, &bgt->linebuf[len - right], right);
  buf += right;

  return buf;
}


static char* _setstatus_begin(bgtask_t* bgt) {
  char* buf = bgt->linebuf + bgt->prefixlen;
  assert(buf+1 < bgt->linebuf + LINEBUF_CAP);
  if (bgt->n == 0) {
    *buf++ = ']';
    *buf++ = ' ';
  } else {
    usize avail = BUFAVAIL(bgt, buf);
    int n;
    if (bgt->ntotal) {
      int digits = ndigits10(bgt->ntotal) * (bgt->flags & BGTASK_FANCY);
      n = snprintf(buf, avail, " %*u/%u] ", digits, bgt->n, MAX(bgt->ntotal, bgt->n));
    } else {
      n = snprintf(buf, avail, " %u] ", bgt->n);
    }
    if (n > 0)
      buf += MIN((usize)n, avail - 1);
  }
  return buf;
}


static void _setstatus_end(bgtask_t* bgt, char* buf) {
  FILE* fp = OUTPUT_FILE;

  usize haslf = (usize)(*(buf-1) == '\n');
  char* bufstart = bgt->linebuf;

  if (bgt->flags & BGTASK_FANCY) {
    // limit to console width
    struct winsize ws;
    if ((ioctl(OUTPUT_FILENO, TIOCGWINSZ, &ws) == 0) && ws.ws_col) {
      usize nnonprint = 1 + haslf; // leading '\r', trailing '\n'
      usize ncol = MIN((usize)ws.ws_col, LINEBUF_CAP - nnonprint);
      usize nprint = (usize)(uintptr)(buf - bgt->linebuf) - nnonprint;
      if (ncol < nprint) {
        buf = _clip_ellipsis(bgt, nprint, ncol);
        *buf = '\n';
        buf += haslf;
      }
    }

    // if nothing has been printed since the previous line,
    // move cursor to beginning of previous line
    if (ftell(fp) == bgt->fpos) {
      // note: linebuf has len(FANCY_START) bytes reserved
      memcpy(bgt->linebuf, FANCY_START, strlen(FANCY_START));
    } else {
      bufstart += strlen(FANCY_START);
    }

    char* minbuf = bgt->linebuf + LINEBUF_CAP - strlen(FANCY_END);
    if (buf > minbuf)
      buf = minbuf;
    memcpy(buf, FANCY_END, strlen(FANCY_END));
    buf += strlen(FANCY_END);
  } else {
    char* lastbyte = buf++;
    if (lastbyte >= bgt->linebuf + LINEBUF_CAP) {
      buf = bgt->linebuf + LINEBUF_CAP;
      lastbyte = buf - 1;
    }
    *lastbyte = '\n';
  }

  assert((uintptr)(buf - bgt->linebuf) <= (uintptr)U32_MAX);
  bgt->len = (u32)(uintptr)(buf - bgt->linebuf);

  usize len = (usize)(uintptr)(buf - bufstart);
  assert(buf < bgt->linebuf + LINEBUF_CAP);
  fwrite(bufstart, len, 1, fp);
  fflush(fp);
  bgt->fpos = ftell(fp);
}


void bgtask_refresh(bgtask_t* bgt) {
  if (bgt->len == 0)
    return;

  FILE* fp = OUTPUT_FILE;
  char* bufstart = bgt->linebuf;
  char* buf = bgt->linebuf + (usize)bgt->len;

  if (bgt->flags & BGTASK_FANCY) {
    if (ftell(fp) != bgt->fpos)
      bufstart += strlen(FANCY_START);
  }

  usize len = (usize)(uintptr)(buf - bufstart);
  assert(buf < bgt->linebuf + LINEBUF_CAP);
  fwrite(bufstart, len, 1, fp);
  fflush(fp);
  bgt->fpos = ftell(fp);
}


void bgtask_setstatus(bgtask_t* bgt, const char* cstr) {
  char* buf = _setstatus_begin(bgt);
  usize len = MIN(BUFAVAIL(bgt, buf), strlen(cstr));
  memcpy(buf, cstr, len);
  _setstatus_end(bgt, buf + len);
}


void bgtask_setstatusv(bgtask_t* bgt, const char* fmt, va_list ap) {
  char* buf = _setstatus_begin(bgt);
  usize cap = BUFAVAIL(bgt, buf);
  int w = vsnprintf(buf, cap, fmt, ap);
  safecheck(w >= 0);
  _setstatus_end(bgt, buf + MIN(cap - 1, (usize)w));
}


void bgtask_setstatusf(bgtask_t* bgt, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  bgtask_setstatusv(bgt, fmt, ap);
  va_end(ap);
}


void bgtask_end(bgtask_t* bgt, const char* fmt, ...) {
  va_list ap;
  usize fmtlen = strlen(fmt);

  char* buf = NULL;
  if (fmtlen || (bgt->flags & BGTASK_FANCY))
    buf = _setstatus_begin(bgt);

  if (fmtlen) {
    va_start(ap, fmt);
    usize cap = BUFAVAIL(bgt, buf);
    int w = vsnprintf(buf, cap, fmt, ap);
    safecheck(w >= 0);
    va_end(ap);
    buf = buf + MIN(cap - 1, (usize)w);
    if (BUFAVAIL(bgt, buf) >= 25+3) {
      *buf++ = ' ';
      *buf++ = '(';
      buf += fmtduration(buf, nanotime() - bgt->start_time);
      *buf++ = ')';
    }
  } else if (bgt->flags & BGTASK_FANCY) {
    usize len = MIN(BUFAVAIL(bgt, buf), strlen(DONE_PREFIX));
    memcpy(buf, DONE_PREFIX, len);
    buf += len;
    if (BUFAVAIL(bgt, buf) >= 25)
      buf += fmtduration(buf, nanotime() - bgt->start_time);
  }

  if (buf)
    _setstatus_end(bgt, buf);
}
