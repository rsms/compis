// SPDX-License-Identifier: Apache-2.0
#ifndef CO_DEFINE_TOKENS
#define CO_DEFINE_TOKENS
  typedef u8 tok_t;
  enum tok {
    #define _(NAME, ...) NAME,
    #define KEYWORD(str, NAME) NAME,
    #include "tokens.h"
    #undef _
    #undef KEYWORD
  };
  enum { TOK_COUNT = (0lu
    #define _(...) + 1lu
    #define KEYWORD(...) + 1lu
    #include "tokens.h"
    #undef _
    #undef KEYWORD
  ) };
#else //—————————————————————————————————————————————————————————————————————————————————

_( TEOF, "eof" )
_( TSEMI, ";" )

_( TLPAREN, "(" ) _( TRPAREN, ")" )
_( TLBRACE, "{" ) _( TRBRACE, "}" )
_( TLBRACK, "[" ) _( TRBRACK, "]" )

_( TDOT,        "." )
_( TDOTDOTDOT,  "..." )
_( TCOLON,      ":" )
_( TCOMMA,      "," )
_( TQUESTION,   "?" )

_( TPLUS,       "+" )
_( TPLUSPLUS,   "++" )
_( TMINUS,      "-" )
_( TMINUSMINUS, "--" )
_( TSTAR,       "*" )
_( TSLASH,      "/" )
_( TPERCENT,    "%" )
_( TTILDE,      "~" )
_( TNOT,        "!" )
_( TAND,        "&" )
_( TANDAND,     "&&" )
_( TOR,         "|" )
_( TOROR,       "||" )
_( TXOR,        "^" )
_( TSHL,        "<<" )
_( TSHR,        ">>" )

_( TEQ,   "==" )
_( TNEQ,  "!=" )

_( TLT,   "<" )
_( TGT,   ">" )
_( TLTEQ, "<=" )
_( TGTEQ, ">=" )

// assignment operators (if this changes, update tok_isassign)
_( TASSIGN,    "=" )
_( TADDASSIGN, "+=" )
_( TSUBASSIGN, "-=" )
_( TMULASSIGN, "*=" )
_( TDIVASSIGN, "/=" )
_( TMODASSIGN, "%=" )
_( TSHLASSIGN, "<<=" )
_( TSHRASSIGN, ">>=" )
_( TANDASSIGN, "&=" )
_( TXORASSIGN, "^=" )
_( TORASSIGN,  "|=" )

_( TCOMMENT, "comment" )
_( TID, "identifier" )
_( TINTLIT, "integer literal" )
_( TFLOATLIT, "number literal" )
_( TBYTELIT, "byte literal" )
_( TSTRLIT, "string literal" )
_( TCHARLIT, "character literal" )

// keywords (must be sorted)
KEYWORD( "else",   TELSE )
KEYWORD( "false",  TFALSE )
KEYWORD( "for",    TFOR )
KEYWORD( "fun",    TFUN )
KEYWORD( "if",     TIF )
KEYWORD( "import", TIMPORT )
KEYWORD( "let",    TLET )
KEYWORD( "mut",    TMUT )
KEYWORD( "pub",    TPUB )
KEYWORD( "return", TRETURN )
KEYWORD( "true",   TTRUE )
KEYWORD( "type",   TTYPE )
KEYWORD( "var",    TVAR )

#undef  KEYWORD_MAXLEN
#define KEYWORD_MAXLEN 6

#endif // CO_DEFINE_TOKENS
