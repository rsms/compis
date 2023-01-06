#ifndef FOREACH_CLI_OPTION
  #error FOREACH_CLI_OPTION not defined
#endif

ASSUME_NONNULL_BEGIN

typedef struct cliopt cliopt_t;
typedef bool(*cli_valload_t)(void* valptr, const char* value);
typedef struct cliopt {
  void*                  valptr;
  const char* nullable   valname;
  cli_valload_t nullable valload;
  const char*            descr;
} cliopt_t;


static bool cli_valload_str(void* valptr, const char* value) {
  dlog("cli_valload_str");
  *(const char**)valptr = value;
  return true;
}


static bool cli_valload_bool(void* valptr, const char* value) {
  *(bool*)valptr = true;
  return true;
}


static cliopt_t options[] = {
  #define _VL(ptr) _Generic((ptr), \
    bool*:        NULL, \
    const char**: cli_valload_str \
  )

  #define _S( p, c, name,          descr) {(void*)(p), NULL, NULL, descr},
  #define _SV(p, c, name, valname, descr) {(void*)(p), valname, _VL(p), descr},
  #define _L( p,    name,          descr) {(void*)(p), NULL, NULL, descr},
  #define _LV(p,    name, valname, descr) {(void*)(p), valname, _VL(p), descr},

  FOREACH_CLI_OPTION(_S, _SV, _L, _LV)
  {0}

  #undef _S
  #undef _SV
  #undef _L
  #undef _LV
  #undef _VL
};


static int dummyvar;

static const struct option longopt_spec[] = {
  #define _S( p, c, name,          descr)  {name, no_argument,       NULL, c},
  #define _SV(p, c, name, valname, descr)  {name, required_argument, NULL, c},
  #define _L( p,    name,          descr)  {name, no_argument,       &dummyvar, 1},
  #define _LV(p,    name, valname, descr)  {name, required_argument, &dummyvar, 1},

  FOREACH_CLI_OPTION(_S, _SV, _L, _LV)
  {0}

  #undef _S
  #undef _SV
  #undef _L
  #undef _LV
};


// returns optind which is the start index into argv of positional arguments.
// e.g. "while (optind < argc) argv[optind++];"
static int parse_cli_options(int argc, char** argv, void(*helpfn)(const char* prog)) {
  int c, i = 0, nerrs = 0, help = 0;

  // e.g. "abc:d:f:h"
  const char optspec[] = {
    #define _S( p, c, name,          descr)  c,
    #define _SV(p, c, name, valname, descr)  c,':',
    #define _IGN(...)

    FOREACH_CLI_OPTION(_S, _SV, _IGN, _IGN)
    0

    #undef _S
    #undef _SV
    #undef _IGN
  };

  while ((c = getopt_long(argc, argv, optspec, longopt_spec, &i)) != -1) {
    help |= c == 'h';
    switch (c) {
      #define _VL(p) _Generic((p), \
        bool*:        cli_valload_bool, \
        const char**: cli_valload_str \
      )
      #define _S( p, c, ...)  case c: *p = true; break;
      #define _SV(p, c, ...)  case c: _VL(p)((void*)p, optarg); break;
      #define _IGN(...)
      FOREACH_CLI_OPTION(_S, _SV, _IGN, _IGN)
      #undef _S
      #undef _SV
      #undef _IGN
      #undef _VL

      case '?':
        // getopt_long already printed an error message
        nerrs++;
        break;

      case ':': warnx("missing value for -%c", optopt); nerrs++; break;

      case 0:
        if (options[i].valload) {
          options[i].valload(options[i].valptr, optarg);
        } else if (options[i].valptr) {
          *(bool*)options[i].valptr = true;
        }
        break;

      default:
        assertf(0, "unhandled cli option -%c", c);
    }
  }

  if (help && helpfn)
    helpfn(argv[0]);

  return nerrs > 0 ? -1 : optind;
}


static void print_options() {
  // calculate description column
  int descr_col = 0;
  int descr_max_col = 30;
  int descr_sep_w = 2; // spaces separating argspec and description
  for (int i = 0; ; i++) {
    struct option o = longopt_spec[i];
    if (o.name == NULL)
      break;
    int w = 6 + (o.name ? 2 + strlen(o.name) : 0);
    // e.g. "  -c  ", "  -c, --name" or "      --name"
    if (options[i].valname)
      w += 1 + strlen(options[i].valname); // " valname"
    w += descr_sep_w;
    if (w > descr_col)
      descr_col = MIN(descr_max_col, w);
  }

  // print
  for (int i = 0; ; i++) {
    struct option o = longopt_spec[i];
    if (o.name == NULL)
      break;
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
    if (options[i].valname)
      printf(" %s", options[i].valname);
    int w = (int)(ftell(stdout) - startcol);
    w += descr_sep_w;
    if (w > descr_col) {
      printf("\n    %s\n", options[i].descr);
    } else {
      printf("%*s%s\n", (descr_col - w) + descr_sep_w, "", options[i].descr);
    }
  }
}

ASSUME_NONNULL_END
