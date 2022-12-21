// SPDX-License-Identifier: Apache-2.0
//
// _( NAME name, opflag_t flags )
//

// temporary flag aliases
#define W  OP_FL_WRITE

// special ops
_( OP_NOOP, 0 )
_( OP_PHI, 0 )
_( OP_ARG, 0 )
_( OP_CALL, 0 )
_( OP_ZERO, 0 ) // zero initializer
_( OP_FUN, 0 )

// constants
_( OP_ICONST, 0 )
_( OP_FCONST, 0 )

// memory
_( OP_VAR, 0 )    // stack memory
_( OP_STORE, 0 )  // T -> T
_( OP_DEREF, 0 )  // *T -> T
_( OP_ALIAS, 0 )  // T -> &T

// ownership & lifetime
_( OP_MOVE, 0 )       // *T -> *T
_( OP_BORROW, 0 )     // T -> &T
_( OP_BORROW_MUT, 0 ) // T -> mut&T
_( OP_DROP, 0 )
_( OP_OCHECK, 0 )     // test if T? has value

// unary
_( OP_INC,  0 ) // ++
_( OP_DEC,  0 ) // --
_( OP_INV,  0 ) // ~
_( OP_NOT,  0 ) // !
_( OP_CAST, 0 ) // T(x)

// binary, arithmetic
_( OP_ADD, 0 ) // +
_( OP_SUB, 0 ) // -
_( OP_MUL, 0 ) // *
_( OP_DIV, 0 ) // /
_( OP_MOD, 0 ) // %

// binary, bitwise
_( OP_AND, 0 ) // &
_( OP_OR, 0 )  // |
_( OP_XOR, 0 ) // ^
_( OP_SHL, 0 ) // <<
_( OP_SHR, 0 ) // >>

// binary, logical
_( OP_LAND, 0 ) // &&
_( OP_LOR, 0 )  // ||

// binary, comparison
_( OP_EQ, 0 )   // ==
_( OP_NEQ, 0 )  // !=
_( OP_LT, 0 )   // <
_( OP_GT, 0 )   // >
_( OP_LTEQ, 0 ) // <=
_( OP_GTEQ, 0 ) // >=

// binary, assignment
_( OP_ASSIGN, 0 )     // =
_( OP_ADD_ASSIGN, 0 ) // +=
_( OP_AND_ASSIGN, 0 ) // &=
_( OP_DIV_ASSIGN, 0 ) // /=
_( OP_MOD_ASSIGN, 0 ) // %=
_( OP_MUL_ASSIGN, 0 ) // *=
_( OP_OR_ASSIGN, 0 )  // |=
_( OP_SHL_ASSIGN, 0 ) // <<=
_( OP_SHR_ASSIGN, 0 ) // >>=
_( OP_SUB_ASSIGN, 0 ) // -=
_( OP_XOR_ASSIGN, 0 ) // ^=


#undef W
