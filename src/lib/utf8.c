#include "utf8.h"

size_t utf8_decode(const char *text, size_t length, int32_t *out) {
    const unsigned char *s = (const unsigned char *)text;
    uint32_t cp;
    uint32_t min;
    size_t n;

    *out = -1;
    if (length == 0) return 0;
    if (s[0] < 0x80) {
        *out = s[0];
        return 1;
    } else if ((s[0] & 0xE0) == 0xC0) {
        n = 2; cp = s[0] & 0x1F; min = 0x80;
    } else if ((s[0] & 0xF0) == 0xE0) {
        n = 3; cp = s[0] & 0x0F; min = 0x800;
    } else if ((s[0] & 0xF8) == 0xF0) {
        n = 4; cp = s[0] & 0x07; min = 0x10000;
    } else {
        return 1;
    }
    if (length < n) return 1;
    for (size_t i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) return 1;
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 1;
    *out = (int32_t)cp;
    return n;
}
