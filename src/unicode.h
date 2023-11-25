// Unicode text
// SPDX-License-Identifier: Apache-2.0
#pragma once
ASSUME_NONNULL_BEGIN

typedef u32 rune_t; // A Unicode codepoint

#define RUNE_SUB     0xFFFDu   // Unicode replacement character
#define RUNE_MAX     0x10FFFFu // Max Unicode codepoint
#define RUNE_INVALID U32_MAX   // Invalid Unicode codepoint
#define RUNE_SELF    0x80u     // runes below this are represented as a single byte

bool rune_isvalid(rune_t r);

// utf8_encode writes to *dst the UTF-8 representation of r,
// advancing *dst by at least one.
// If r is an invalid Unicode codepoint (i.e. r>RUNE_MAX) RUNE_SUB is used instead.
// Returns false if there's not enough space at *dst.
bool utf8_encode(u8** dst, const u8* dstend, rune_t r);

// utf8_decode validates and decodes the next codepoint at *src.
// Required precondition: *src < end (input is not empty.)
// Always advances *src by at least 1 byte.
// If src is a partial valid sequence (underflow), *src is set to end
// and false is returned.
// Returns RUNE_INVALID if *src contains invalid UTF-8 data.
// If RUNE_INVALID is returned, caller should use RUNE_SUB.
rune_t utf8_decode(const u8** src, const u8* end);

ASSUME_NONNULL_END
