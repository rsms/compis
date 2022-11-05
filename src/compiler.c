#include "c0lib.h"
#include "compiler.h"


void compiler_init(compiler_t* c, memalloc_t ma, diaghandler_t dh) {
  memset(c, 0, sizeof(*c));
  c->ma = ma;
  c->triple = "";
  c->diaghandler = dh;
  buf_init(&c->diagbuf, ma);
}


void compiler_dispose(compiler_t* c) {
  buf_dispose(&c->diagbuf);
}
