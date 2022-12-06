// SPDX-License-Identifier: Apache-2.0

// special ops
_( OP_NOOP )
_( OP_PHI )
_( OP_ARG )
_( OP_CALL )
_( OP_ZERO ) // zero initializer

// constants
_( OP_CONST_BOOL )
_( OP_CONST_I8 )
_( OP_CONST_I16 )
_( OP_CONST_I32 )
_( OP_CONST_I64 )
_( OP_CONST_F32 )
_( OP_CONST_F64 )

// memory
_( OP_LOCAL ) // stack memory
_( OP_STORE )

// unary
_( OP_INC )    // ++
_( OP_DEC )    // --
_( OP_INVERT ) // ~
_( OP_NOT )    // !
// _( OP_DEREF )  // *

// binary, arithmetic
_( OP_ADD ) // +
_( OP_SUB ) // -
_( OP_MUL ) // *
_( OP_DIV ) // /
_( OP_MOD ) // %

// binary, bitwise
_( OP_AND ) // &
_( OP_OR )  // |
_( OP_XOR ) // ^
_( OP_SHL ) // <<
_( OP_SHR ) // >>

// binary, logical
_( OP_LAND ) // &&
_( OP_LOR )  // ||

// binary, comparison
_( OP_EQ )   // ==
_( OP_NEQ )  // !=
_( OP_LT )   // <
_( OP_GT )   // >
_( OP_LTEQ ) // <=
_( OP_GTEQ ) // >=

// binary, assignment
_( OP_ASSIGN )     // =
_( OP_ADD_ASSIGN ) // +=
_( OP_AND_ASSIGN ) // &=
_( OP_DIV_ASSIGN ) // /=
_( OP_MOD_ASSIGN ) // %=
_( OP_MUL_ASSIGN ) // *=
_( OP_OR_ASSIGN )  // |=
_( OP_SHL_ASSIGN ) // <<=
_( OP_SHR_ASSIGN ) // >>=
_( OP_SUB_ASSIGN ) // -=
_( OP_XOR_ASSIGN ) // ^=
