#ifndef SOSU_UTF8_H
#define SOSU_UTF8_H

#include "common.h"

/* Decodes one code point from `text` (at most `length` bytes).
   Returns the number of bytes consumed (at least 1 when length > 0) and stores the
   code point in *out, or -1 if the sequence is invalid (overlong, surrogate, out of
   range or truncated); an invalid sequence consumes a single byte. */
size_t utf8_decode(const char *text, size_t length, int32_t *out);

#endif
