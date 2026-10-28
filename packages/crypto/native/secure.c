/* crypto.secure: random bytes from the system, for keys and tokens (spec §8.2). Gives 1, or 0
   when the system gives none. */
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>

int32_t sosu_crypto_random(uint8_t *buffer, size_t size) {
    while (size > 0) {
        ULONG n = size > 0x40000000u ? 0x40000000u : (ULONG)size;
        if (BCryptGenRandom(NULL, buffer, n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return 0;
        buffer += n;
        size -= n;
    }
    return 1;
}

#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
/* Declared here: strict C99 may hide it in <stdlib.h>. */
void arc4random_buf(void *buffer, size_t size);

int32_t sosu_crypto_random(uint8_t *buffer, size_t size) {
    arc4random_buf(buffer, size);
    return 1;
}

#else
#include <stdio.h>

/* Linux and Android: the generator of the kernel, which never blocks once seeded. */
int32_t sosu_crypto_random(uint8_t *buffer, size_t size) {
    FILE *file = fopen("/dev/urandom", "rb");
    if (!file) return 0;
    setvbuf(file, NULL, _IONBF, 0);
    size_t n = fread(buffer, 1, size, file);
    fclose(file);
    return n == size ? 1 : 0;
}
#endif
