// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ast.h"
#include "bits.h"
ASSUME_NONNULL_BEGIN

typedef u8 irflag_t;
#define IR_FL_SEALED ((irflag_t)1<< 0) // [block] is sealed

typedef u8 irblockkind_t;
enum irblockkind {
  IR_BLOCK_GOTO = 0, // plain continuation block with a single successor
  IR_BLOCK_RET,      // no successors, control value is memory result
  IR_BLOCK_SWITCH,   // N successors, switch(control) goto succs[N]
};

typedef struct irval_ irval_t;
typedef struct irval_ {
  u32      id;
  u32      nuse;
  irflag_t flags;
  op_t     op;
  u8       _reserved[2];
  u32      argc;
  irval_t* argv[3];
  loc_t    loc;
  type_t*  type;
  union {
    u32            i32val;
    u64            i64val;
    f32            f32val;
    f64            f64val;
    void* nullable ptr;
    slice_t        bytes; // pointer into AST node, e.g. strlit_t
  } aux;
  union {
    u8* nullable dead_members; // used by livevars to track dead members
    struct {
      sym_t nullable live;
      sym_t nullable dst;
      sym_t nullable src;
    } var;
  };

  const char* nullable comment;
} irval_t;

typedef struct irblock_ irblock_t;
typedef struct irblock_ {
  u32                 id;
  irflag_t            flags;
  irblockkind_t       kind;
  u8                  _reserved[2];
  loc_t               loc;
  irblock_t* nullable succs[2]; // successors (CFG)
  irblock_t* nullable preds[2]; // predecessors (CFG)
  ptrarray_t          values;
  irval_t* nullable   control;
    // control is a value that determines how the block is exited.
    // Its value depends on the kind of the block.
    // I.e. a IR_BLOCK_SWITCH has a boolean control value
    // while a IR_BLOCK_RET has a memory control value.
  const char* nullable comment;
} irblock_t;

typedef struct {
  fun_t*      ast;
  const char* name;
  ptrarray_t  blocks;
  u32         bidgen;     // block id generator
  u32         vidgen;     // value id generator
  u32         ncalls;     // # function calls that this function makes
  u32         npurecalls; // # function calls to functions marked as "pure"
  u32         nglobalw;   // # writes to globals
} irfun_t;

typedef struct {
  ptrarray_t          functions;
  srcfile_t* nullable srcfile;
} irunit_t;


bool irfmt(compiler_t*, const pkg_t*, buf_t*, const irunit_t*);
bool irfmt_dot(compiler_t*, const pkg_t*, buf_t*, const irunit_t*);
bool irfmt_fun(compiler_t*, const pkg_t*, buf_t*, const irfun_t*);

bool dead_members_has(
  const u8* nullable dead_members, usize member_count, usize member_index);


ASSUME_NONNULL_END
