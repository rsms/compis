// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "iniparse.h"
#include "userconfig.h"

#include <stdlib.h>
#include <sys/stat.h>


static char g_zeroch = 0;

static userconfig_t* g_uconf = NULL; // global config, for all targets

static char*        g_targetpat_patternv[SUPPORTED_TARGETS_COUNT*2];
static userconfig_t g_targetpat_uconfv[SUPPORTED_TARGETS_COUNT*2] = {};
static u32          g_targetpat_uconfc = 0;

// canonical config per target
static userconfig_t* g_target_confv[SUPPORTED_TARGETS_COUNT] = {};


#define UCONF_FOREACH_FIELD(STR) \
  /* ( FIELD, const char* config_name )*/ \
  STR(sysroot, "sysroot") \
  STR(linkflags, "linkflags") \
// end UCONF_FOREACH_FIELD


enum { UCONF_FIELD_COUNT = 0lu UCONF_FOREACH_FIELD(CO_PLUS_ONE) };


static userconfig_t* nullable uconf_copy(memalloc_t ma, const userconfig_t* other) {
  // calculate size needed for strings
  usize z = 0;
  #define _STR(FIELD, ...) z += strlen(other->FIELD) + 1;
  UCONF_FOREACH_FIELD(_STR)
  #undef _STR

  userconfig_t* uconf = mem_alloc(memalloc_ctx(), sizeof(userconfig_t) + z).p;
  if (!uconf)
    return NULL;

  // copy strings
  char* strings = (char*)uconf + sizeof(userconfig_t);

  #define _STR(FIELD, ...) \
    uconf->FIELD = strings; \
    z = strlen(other->FIELD) + 1; \
    memcpy(uconf->FIELD, other->FIELD, z); \
    strings += z;
  UCONF_FOREACH_FIELD(_STR)
  #undef _STR

  return uconf;
}


static void uconf_merge(userconfig_t* uconf, const userconfig_t* other) {
  #define _STR(FIELD, ...) \
    if (other->FIELD) { \
      uconf->FIELD = mem_strdup( \
        memalloc_default(), \
        (slice_t){ .chars=other->FIELD, .len=strlen(other->FIELD) }, 0); \
    }
  UCONF_FOREACH_FIELD(_STR)
  #undef _STR
}


const userconfig_t* userconfig_generic() {
  assertnotnull(g_uconf); // allocated by userconfig_load
  return g_uconf;
}


const userconfig_t* userconfig_for_target(const target_t* target) {
  if (g_targetpat_uconfc == 0)
    return g_uconf;

  usize supported_targets_idx = 0;
  for (; supported_targets_idx < SUPPORTED_TARGETS_COUNT; supported_targets_idx++) {
    if (strcmp(supported_targets[supported_targets_idx].triple, target->triple) == 0) {
      if (g_target_confv[supported_targets_idx])
        return g_target_confv[supported_targets_idx];
      break;
    }
  }

  userconfig_t* uconf = uconf_copy(memalloc_default(), g_uconf);

  char targetstr[TARGET_FMT_BUFCAP];
  target_fmt(target, targetstr, sizeof(targetstr));

  for (u32 i = 0; i < g_targetpat_uconfc; i++) {
    if (target_str_match(targetstr, g_targetpat_patternv[i]))
      uconf_merge(uconf, &g_targetpat_uconfv[i]);
  }

  if (supported_targets_idx < SUPPORTED_TARGETS_COUNT)
    g_target_confv[supported_targets_idx] = uconf;

  return uconf;
}


static userconfig_t* nullable target_uconf_get(
  const char* target_pattern, usize target_pattern_len)
{
  // look for existing
  for (u32 i = 0; i < g_targetpat_uconfc; i++) {
    usize pattern_len = strlen(g_targetpat_patternv[i]);
    if (pattern_len == target_pattern_len &&
        memcmp(g_targetpat_patternv[i], target_pattern, pattern_len) == 0)
    {
      return &g_targetpat_uconfv[i];
    }
  }
  // allocate
  if (g_targetpat_uconfc >= countof(g_targetpat_uconfv))
    return NULL;
  char* pattern = mem_strdup(
    memalloc_default(), (slice_t){ .chars=target_pattern, .len=target_pattern_len }, 0);
  if (pattern == NULL)
    return NULL;
  g_targetpat_patternv[g_targetpat_uconfc] = pattern;
  return &g_targetpat_uconfv[g_targetpat_uconfc++];
}


static const struct {
  u32         offs;
  u32         keylen;
  const char* key;
} g_uconf_fields[] = {
  #define _STR(FIELD, KEY) { offsetof(userconfig_t, FIELD), strlen(KEY), KEY },
  UCONF_FOREACH_FIELD(_STR)
  #undef _STR
};


static bool userconfig_load1(const char* srcfile, const char* src, usize srclen) {
  iniparse_t p;
  iniparse_begin(&p, src, srclen);
  userconfig_t* uconf = g_uconf;

  for (iniparse_result_t r; (r = iniparse_next(&p));) switch (r) {
    case INIPARSE_END: UNREACHABLE; break;
    case INIPARSE_SECTION:
      // dlog("SECTION \"%.*s\"", (int)p.namelen, p.name);
      uconf = target_uconf_get(p.name, p.namelen);
      if (!uconf) {
        elog("%s: warning: too many target sections (max=%u)",
          srcfile, (u32)countof(g_targetpat_uconfv));
        return true;
      }
      break;
    case INIPARSE_VALUE: {
      // dlog("VALUE   \"%.*s\" = \"%.*s\"",
      //   (int)p.namelen, p.name, (int)p.valuelen, p.value);
      for (usize i = 0;;i++) {
        if UNLIKELY(i == countof(g_uconf_fields)) {
          elog("%s:%u: unknown key \"%.*s\" ignored",
            srcfile, p.srcline, (int)p.namelen, p.name);
          break;
        }
        if (g_uconf_fields[i].keylen == p.namelen &&
            memcmp(g_uconf_fields[i].key, p.name, p.namelen) == 0)
        {
          char* value = p.valuelen == 0 ? &g_zeroch : mem_strdup(
            memalloc_default(), (slice_t){ .chars=p.value, .len=p.valuelen }, 0);
          *(char**)((void*)uconf + g_uconf_fields[i].offs) = value;
          break;
        }
      }
      break;
    }
    case INIPARSE_COMMENT:
      // dlog("COMMENT \"%.*s\"", (int)p.valuelen, p.value);
      break;
    case INIPARSE_ERR_SYNTAX:
      elog("%s:%u: syntax error", srcfile, p.srcline);
      return false;
  }
  return true;
}


void userconfig_load(int argc, char* argv[]) {
  const void* data;
  struct stat st;
  err_t err;

  const char* filenames[] = {
    getenv("COMPIS_USERCONFIG"),

    #ifdef __linux__
    "/etc/compis.conf",
    #endif

    #ifdef __APPLE__
    strcat_alloca(sys_homedir(), "/.compis.conf"),
    #endif

    strcat_alloca(coroot, "/default.conf"),
  };

  // create default global config
  g_uconf = mem_alloc(memalloc_default(), sizeof(userconfig_t) + UCONF_FIELD_COUNT).p;
  if (!g_uconf)
    panic("mem_alloc");
  char* strings = (char*)g_uconf + sizeof(userconfig_t);
  memset(strings, 0, UCONF_FIELD_COUNT);
  #define _STR(NAME, ...) g_uconf->NAME = strings; strings += 1;
  UCONF_FOREACH_FIELD(_STR)
  #undef _STR

  // load config files
  for (usize i = 0; i < countof(filenames); i++) {
    const char* srcfile = filenames[i];
    if (!srcfile || !*srcfile)
      continue;
    err = mmap_file_ro(srcfile, &data, &st);
    if (err) {
      if (err == ErrNotFound) {
        vvlog("[userconfig] %s skipped (not found)", srcfile);
      } else {
        elog("%s: %s", srcfile, err_str(err));
      }
      continue;
    }
    // TODO: test if it's a regular file
    vvlog("[userconfig] loading %s", srcfile);
    bool ok = userconfig_load1(srcfile, data, st.st_size);
    mmap_unmap(&data, st.st_size);
    if (ok)
      break;
  }

  // for (usize i = 0; i < SUPPORTED_TARGETS_COUNT; i++) {
  //   const userconfig_t* uconf = userconfig_for_target(&supported_targets[i]);
  //   char tmpbuf[TARGET_FMT_BUFCAP];
  //   target_fmt(&supported_targets[i], tmpbuf, sizeof(tmpbuf));
  //   dlog(">> %s\t= %p", tmpbuf, uconf);
  //   #define _STR(FIELD, ...) dlog("  " #FIELD "\t= %s", uconf->FIELD);
  //   UCONF_FOREACH_FIELD(_STR)
  //   #undef _STR
  // }
}
