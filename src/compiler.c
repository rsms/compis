#include "c0lib.h"
#include "compiler.h"
#include "path.h"


static void set_cachedir(compiler_t* c, slice_t cachedir) {
  c->cachedir = mem_strdup(c->ma, cachedir, 0);

  const char* objdir_name = "obj";
  // poor coder's path_join(cachedir, "obj"):
  c->objdir = mem_strdup(c->ma, cachedir, 1 + strlen(objdir_name));
  c->objdir[cachedir.len] = PATH_SEPARATOR;
  memcpy(c->objdir + cachedir.len + 1, objdir_name, strlen(objdir_name));
  c->objdir[cachedir.len + 1 + strlen(objdir_name)] = 0;
}


void compiler_init(compiler_t* c, memalloc_t ma, diaghandler_t dh) {
  memset(c, 0, sizeof(*c));
  c->ma = ma;
  c->triple = "";
  c->diaghandler = dh;
  buf_init(&c->diagbuf, ma);
  set_cachedir(c, slice_cstr(".c0"));
}


void compiler_dispose(compiler_t* c) {
  buf_dispose(&c->diagbuf);
}


void compiler_set_cachedir(compiler_t* c, slice_t cachedir) {
  mem_freex(c->ma, MEM(c->cachedir, strlen(c->cachedir) + 1));
  mem_freex(c->ma, MEM(c->objdir, strlen(c->objdir) + 1));
  set_cachedir(c, cachedir);
}
