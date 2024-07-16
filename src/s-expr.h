#pragma once
#include "buf.h"
ASSUME_NONNULL_BEGIN

typedef struct s_expr_t {
  struct s_expr_t* nullable next;
  enum { SEXPR_LIST, SEXPR_ATOM } type;
} s_expr_t;

typedef struct s_expr_list_t {
  s_expr_t;
  u8                kind; // one of: . ( [ {
  s_expr_t* nullable head;
} s_expr_list_t;

typedef struct s_expr_atom_t {
  s_expr_t;
  union {
    slice_t slice;
    struct {
      const char* chars;
      usize       len;
    };
  };
} s_expr_atom_t;

enum s_expr_fmt_flags {
  SEXPR_FMT_PRETTY = 1 << 0, // separate values with linebreaks and indentation
};

typedef struct s_expr_diag_t {
  enum { SEXPR_DIAG_ERR } kind;
  u32                  line;
  u32                  col;
  const char*          message;
} s_expr_diag_t;

typedef void (*s_expr_diag_handler_t)(const s_expr_diag_t* diag, void* nullable userdata);

err_t s_expr_parse(
  s_expr_list_t**                result_out,
  slice_t                       src,
  memalloc_t                    ma,
  s_expr_diag_handler_t nullable diag_handler,
  void* nullable                userdata);

void s_expr_free(s_expr_list_t* n, memalloc_t ma);
err_t s_expr_fmt(const void* /*s_expr_list_t|s_expr_atom_t*/ n, buf_t* buf, u32 flags);
err_t s_expr_prettyprint(buf_t* dst, slice_t src);
s_expr_t* nullable s_expr_at(s_expr_list_t* list, usize index);
s_expr_atom_t* s_expr_atom_at(s_expr_list_t* list, usize index);
s_expr_list_t* s_expr_list_at(s_expr_list_t* list, usize index);

#define SEXPR_LIST_FOREACH(list, MEMBER_VARNAME) \
  for (s_expr_t* __l##__LINE__ = (s_expr_t*)_Generic((list),s_expr_list_t*:(list)), \
       *MEMBER_VARNAME = ((s_expr_list_t*)__l##__LINE__)->head; \
       MEMBER_VARNAME != NULL; \
       MEMBER_VARNAME = MEMBER_VARNAME->next)

#define SEXPR_TYPECAST(n) _Generic((n), \
  const s_expr_list_t*: ((const s_expr_t*)(n)), \
  const s_expr_atom_t*: ((const s_expr_t*)(n)), \
  const s_expr_t*: (n), \
  s_expr_list_t*: ((s_expr_t*)(n)), \
  s_expr_atom_t*: ((s_expr_t*)(n)), \
  s_expr_t*: (n) \
)

//——————————————————————————————————————————————————————————————————————————————————————
// implementation

err_t _s_expr_fmt(const s_expr_t* n, buf_t* buf, u32 flags);
#define s_expr_fmt(n, buf, flags) _s_expr_fmt(SEXPR_TYPECAST(n), (buf), (flags))

ASSUME_NONNULL_END
