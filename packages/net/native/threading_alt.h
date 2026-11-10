/* The mutexes of Mbed TLS on Windows (MBEDTLS_THREADING_ALT, sosu_crypto_config.h): slim
   reader/writer locks and condition variables, given to mbedtls_threading_set_alt by tls.c. */
#ifndef SOSU_THREADING_ALT_H
#define SOSU_THREADING_ALT_H

/* without winsock.h, which would clash with the winsock2.h of a file included later */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef SRWLOCK mbedtls_platform_mutex_t;
typedef CONDITION_VARIABLE mbedtls_platform_condition_variable_t;

#endif
