# Updating Mbed TLS

Mbed TLS fixes security issues several times a year: `net` must follow its releases of the
long-term support branch it uses (4.1, until March 2029), and move to the next one before.

1. Download `mbedtls-<version>.tar.bz2` and `mbedtls-<version>-sha256sum.txt` from
   <https://github.com/Mbed-TLS/mbedtls/releases>, and check the SHA-256.
2. Replace `mbedtls/` with the same parts of the archive: `LICENSE`, `README.md`, `include/`,
   `library/*.c` and `library/*.h`, and of `tf-psa-crypto/`: `LICENSE`, `include/`, `core/`,
   `dispatch/`, `extras/`, `platform/`, `utilities/` (their `.c` and `.h`) and
   `drivers/builtin/` (`include/` and `src/`). Remove the `CMakeLists.txt` files.
3. Add back, at the end of `include/mbedtls/mbedtls_config.h` and before the last `#endif` of
   `tf-psa-crypto/include/psa/crypto_config.h`, the lines that include `sosu_tls_config.h` and
   `sosu_crypto_config.h`.
4. Update the version and the SHA-256 in `THIRD_PARTY_NOTICES.md`, the comments of `sosu.toml`
   and `tls.c`, then run `sosuc test tests/programs/network` on Windows, Linux and macOS (the
   CI does), and an https request to a real server.
