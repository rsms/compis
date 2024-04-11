// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "target.h"
ASSUME_NONNULL_BEGIN

typedef struct userconfig_t {
  char* linkflags;
  char* sysroot;
} userconfig_t;

const userconfig_t* userconfig_generic();
const userconfig_t* userconfig_for_target(const target_t* target);
void userconfig_load(int argc, char* argv[]);

ASSUME_NONNULL_END
