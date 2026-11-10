/* The choices of Sosu for TF-PSA-Crypto, read at the end of
   mbedtls/tf-psa-crypto/include/psa/crypto_config.h (the one line added to it). */

/* No key is kept in files: nothing is written to the disk. */
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_PSA_ITS_FILE_C
/* What TLS does not use. */
#undef MBEDTLS_SELF_TEST
#undef MBEDTLS_LMS_C
#undef MBEDTLS_NIST_KW_C

/* Sosu programs have threads (spec 5.6): the state of the library is behind mutexes, those of
   pthread or, on Windows, those of threading_alt.h. */
#define MBEDTLS_THREADING_C
#if defined(_WIN32)
#define MBEDTLS_THREADING_ALT
#else
#define MBEDTLS_THREADING_PTHREAD
#endif
