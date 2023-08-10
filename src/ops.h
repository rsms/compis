// SPDX-License-Identifier: Apache-2.0
#ifndef CO_DEFINE_OPS
#define CO_DEFINE_OPS
  // operation codes
  typedef u8 op_t;
  enum op {
    #define _(NAME, ...) NAME,
    #include "ops.h"
    #undef _
  };
  const char* op_fmt(op_t op);  // e.g. "+"
  const char* op_name(op_t op); // e.g. "ADD"
  int op_name_maxlen();

#elif !defined(CO_INCLUDE_OPS) //————————————————————————————————————————————————————
// _( NAME name, const char* fmtstr )

// temporary flag aliases
#define W  OP_FL_WRITE

// special ops
_( OP_NOOP, "" )
_( OP_PHI,  "" )
_( OP_ARG,  "" )
_( OP_CALL, "" )
_( OP_ZERO, "" ) // zero initializer
_( OP_FUN,  "" )

// constants
_( OP_ICONST, "" )
_( OP_FCONST, "" )

// literals
_( OP_ARRAY, "" ) // [v ...]
_( OP_STR,   "" ) // "..."

// memory
_( OP_VAR,   "" ) // stack memory
_( OP_STORE, "" ) // T -> T
_( OP_DEREF, "" ) // *T -> T
_( OP_ALIAS, "" ) // T -> *T
_( OP_GEP,   "" ) // get element pointer

// ownership & lifetime
_( OP_MOVE,   "" ) // T -> T
_( OP_REF,    "" ) // T -> &T
_( OP_MUTREF, "" ) // T -> mut&T
_( OP_DROP,   "" ) //
_( OP_OCHECK, "" ) // test if T? has value

// unary
_( OP_INC,  "++" )
_( OP_DEC,  "--" )
_( OP_INV,  "~" )
_( OP_NOT,  "!" )
_( OP_CAST, "cast" )

// binary, arithmetic
_( OP_ADD, "+" )
_( OP_SUB, "-" )
_( OP_MUL, "*" )
_( OP_DIV, "/" )
_( OP_MOD, "%" )

// binary, bitwise
_( OP_AND, "&" )
_( OP_OR,  "|" )
_( OP_XOR, "^" )
_( OP_SHL, "<<" )
_( OP_SHR, ">>" )

// binary, logical
_( OP_LAND, "&&" )
_( OP_LOR,  "||" )

// binary, comparison
_( OP_EQ,   "==" )
_( OP_NEQ,  "!=" )
_( OP_LT,   "<" )
_( OP_GT,   ">" )
_( OP_LTEQ, "<=" )
_( OP_GTEQ, ">=" )

// binary, assignment
_( OP_ASSIGN,     "=" )
_( OP_ADD_ASSIGN, "+=" )
_( OP_SUB_ASSIGN, "-=" )
_( OP_MUL_ASSIGN, "*=" )
_( OP_DIV_ASSIGN, "/=" )
_( OP_MOD_ASSIGN, "%=" )
_( OP_AND_ASSIGN, "&=" )
_( OP_OR_ASSIGN,  "|=" )
_( OP_XOR_ASSIGN, "^=" )
_( OP_SHL_ASSIGN, "<<=" )
_( OP_SHR_ASSIGN, ">>=" )


#undef W
#endif // CO_DEFINE_OPS or CO_INCLUDE_OPS
