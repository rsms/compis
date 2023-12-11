#ifndef FOREACH_CLI_OPTION
  #error FOREACH_CLI_OPTION not defined
#endif
#include <getopt.h>

ASSUME_NONNULL_BEGIN

typedef struct cliopt cliopt_t;
typedef bool(*cli_valload_t)(void* valptr, const char* value);
typedef struct cliopt {
  void*                  valptr;
  const char* nullable   valname;
  cli_valload_t nullable valload;
  const char*            descr;
#ifdef DEBUG
  bool                   isdebug;
#endif
} cliopt_t;


typedef struct cliopt_args_t {
  char* const* argv;
  int          argc;
} cliopt_args_t;


// cliopt_parse parses command line arguments.
// When this function returns, argc & argv points to an array of non-option arguments.
// Returns false if an error occurred.
static bool cliopt_parse(int* argc, char** argv[], void(*helpfn)(const char* prog));

// cliopt_print prints a summary of all command options
static void cliopt_print();

// ———————————————————————————————————————————————————————————————————————————————————

UNUSED static bool cli_valload_str(void* valptr, const char* value) {
  *(const char**)valptr = value;
  return true;
}

UNUSED static bool cli_valload_bool(void* valptr, const char* value) {
  *(bool*)valptr = true;
  return true;
}


static void cli_set_bool(bool* valptr) {
  *valptr = true;
}

static void cli_set_intbool(int* valptr) {
  (*valptr)++;
}


static cliopt_t g_cli_options[] = {
  #define _VL(ptr) _Generic((ptr), \
    bool*:        NULL, \
    const char**: cli_valload_str \
  )

  #define _S( p, c, name,          descr) {(void*)(p), NULL, NULL, descr},
  #define _SV(p, c, name, valname, descr) {(void*)(p), valname, _VL(p), descr},
  #define _L( p,    name,          descr) {(void*)(p), NULL, NULL, descr},
  #define _LV(p,    name, valname, descr) {(void*)(p), valname, _VL(p), descr},
  #ifdef DEBUG
    #define _DL( p,    name,          descr) {(void*)(p), NULL, NULL, descr, 1},
    #define _DLV(p,    name, valname, descr) {(void*)(p), valname, _VL(p), descr, 1},
  #else
    #define _DL( p,    name,          descr)
    #define _DLV(p,    name, valname, descr)
  #endif

  FOREACH_CLI_OPTION(_S, _SV, _L, _LV, _DL, _DLV)

  #undef _S
  #undef _SV
  #undef _L
  #undef _LV
  #undef _DL
  #undef _DLV

  #undef _VL
};


static int dummyvar;

static const struct option longopt_spec[] = {
  #define _S( p, c, name,          descr)  {name, no_argument,       NULL, c},
  #define _SV(p, c, name, valname, descr)  {name, required_argument, NULL, c},
  #define _L( p,    name,          descr)  {name, no_argument,       &dummyvar, 1},
  #define _LV(p,    name, valname, descr)  {name, required_argument, &dummyvar, 1},
  #ifdef DEBUG
    #define _DL( p,    name,          descr)  {name, no_argument,       &dummyvar, 1},
    #define _DLV(p,    name, valname, descr)  {name, required_argument, &dummyvar, 1},
  #else
    #define _DL( p,    name,          descr)
    #define _DLV(p,    name, valname, descr)
  #endif

  FOREACH_CLI_OPTION(_S, _SV, _L, _LV, _DL, _DLV)
  {0}

  #undef _S
  #undef _SV
  #undef _L
  #undef _LV
  #undef _DL
  #undef _DLV
};

/*typedef struct cliopt_args_t {
  char* const argv[];
  int         argc;
} cliopt_args_t;*/


static bool cliopt_parse(int* argcp, char** argvp[], void(*helpfn)(const char* prog)) {
  int argc = *argcp;
  char** argv = *argvp;
  int c, i = 0, nerrs = 0, help = 0;

  // e.g. "abc:d:f:h"
  const char optspec[] = {
    #define _S( p, c, name,          descr)  c,
    #define _SV(p, c, name, valname, descr)  c,':',
    #define _IGN(...)

    FOREACH_CLI_OPTION(_S, _SV, _IGN, _IGN, _IGN, _IGN)
    0

    #undef _S
    #undef _SV
    #undef _IGN
  };

  while ((c = getopt_long(argc, argv, optspec, longopt_spec, &i)) != -1) {
    help |= c == 'h';
    switch (c) {
      #define _UPDATEPTR(p) _Generic((p), \
        bool*:        cli_valload_bool, \
        const char**: cli_valload_str \
      )
      #define _SETBOOL(p) _Generic((p), \
        bool*: cli_set_bool, \
        int*:  cli_set_intbool \
      )
      #define _S( p, c, ...)  case c: _SETBOOL(p)((void*)p); break;
      #define _SV(p, c, ...)  case c: _UPDATEPTR(p)((void*)p, optarg); break;
      #define _IGN(...)
      FOREACH_CLI_OPTION(_S, _SV, _IGN, _IGN, _IGN, _IGN)
      #undef _S
      #undef _SV
      #undef _IGN
      #undef _UPDATEPTR

      case '?':
        // getopt_long already printed an error message
        nerrs++;
        break;

      case ':': elog("%s: missing value for -%c", coprogname, optopt); nerrs++; break;

      case 0:
        if (g_cli_options[i].valload) {
          g_cli_options[i].valload(g_cli_options[i].valptr, optarg);
        } else if (g_cli_options[i].valptr) {
          *(bool*)g_cli_options[i].valptr = true;
        }
        break;

      default:
        assertf(0, "unhandled cli option -%c", c);
    }
  }

  if (help && helpfn)
    helpfn(argv[0]);

  if (nerrs)
    return false;

  // getopt_long has shuffled argv around to that all non-option arguments
  // are at the end, starting at optind
  *argvp = *argvp + optind;
  *argcp = *argcp - optind;

  return true;
}


static void cliopt_print1(bool isdebug) {
  // calculate description column
  int descr_col = 0;
  int descr_max_col = 30;
  int descr_sep_w = 2; // spaces separating argspec and description
  for (usize i = 0; i < countof(g_cli_options); i++) {
    #if DEBUG
      if (g_cli_options[i].isdebug != isdebug)
        continue;
    #endif
    struct option o = longopt_spec[i];
    assertnotnull(o.name);
    int w = 6 + (o.name ? 2 + strlen(o.name) : 0);
    // e.g. "  -c  ", "  -c, --name" or "      --name"
    if (g_cli_options[i].valname)
      w += 1 + strlen(g_cli_options[i].valname); // " valname"
    w += descr_sep_w;
    if (w > descr_col)
      descr_col = MIN(descr_max_col, w);
  }

  // print
  for (usize i = 0; i < countof(g_cli_options); i++) {
    #if DEBUG
      if (g_cli_options[i].isdebug != isdebug)
        continue;
    #endif
    struct option o = longopt_spec[i];
    long startcol = ftell(stdout);
    if (o.flag == NULL) {
      if (strlen(o.name) == 0) {
        printf("  -%c  ", o.val);
      } else {
        printf("  -%c, --%s", o.val, o.name);
      }
    } else {
      printf("      --%s", o.name);
    }
    if (g_cli_options[i].valname)
      printf(" %s", g_cli_options[i].valname);
    int w = (int)(ftell(stdout) - startcol);
    w += descr_sep_w;
    if (w > descr_col) {
      printf("\n    %s\n", g_cli_options[i].descr);
    } else {
      printf("%*s%s\n", (descr_col - w) + descr_sep_w, "", g_cli_options[i].descr);
    }
  }
}


static void cliopt_print() {
  cliopt_print1(false);

  // if there are debug-build options, print them separately
  #if DEBUG
    for (usize i = 0; i < countof(g_cli_options); i++) {
      if (g_cli_options[i].isdebug) {
        printf("Options only available in compis debug build:\n");
        cliopt_print1(true);
        break;
      }
    }
  #endif
}

ASSUME_NONNULL_END
