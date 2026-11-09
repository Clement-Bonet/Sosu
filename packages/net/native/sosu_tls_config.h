/* The choices of Sosu for Mbed TLS, read at the end of
   mbedtls/include/mbedtls/mbedtls_config.h (the one line added to it). */

/* net.tls reads and writes through the sockets of native/net.c, and keeps no session. */
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_DEBUG_C
#undef MBEDTLS_SSL_CACHE_C
#undef MBEDTLS_SSL_COOKIE_C
#undef MBEDTLS_SSL_TICKET_C
#undef MBEDTLS_SSL_CONTEXT_SERIALIZATION
#undef MBEDTLS_SSL_RENEGOTIATION
#undef MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE
#undef MBEDTLS_PKCS7_C
