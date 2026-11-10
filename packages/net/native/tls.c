/* net.tls: TLS 1.2 and 1.3 with Mbed TLS 4.1 (mbedtls/, Apache 2.0, see THIRD_PARTY_NOTICES.md),
   over the sockets of net.c, behind small functions of plain C types for src/tls.sosu.

   Certificates are checked against the roots given by the program (PEM), or else against those
   of the system: the store ROOT of Windows, the bundle of the distribution on Linux, the folders
   of Android. On macOS and iOS the system checks the chain itself (SecTrust), with the same
   rules as Safari. A function that fails sets `*error` to one of the TLS_ codes below, and
   `*net_error` to the code of net.c when the network failed. */
/* Before any #include: glibc reads it at the first one. */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/threading.h"
#include "psa/crypto.h"

enum {
    TLS_OK = 0,
    TLS_NETWORK = 1,     /* the connection failed: *net_error says how */
    TLS_UNTRUSTED = 2,   /* the certificate is not signed by a trusted root */
    TLS_EXPIRED = 3,     /* the certificate is out of its dates */
    TLS_WRONG_NAME = 4,  /* the certificate is for another name */
    TLS_HANDSHAKE = 5,   /* no common version or cipher, an alert, a broken message */
    TLS_INVALID = 6,     /* a certificate or a key in PEM that cannot be read */
    TLS_OTHER = 7,
};

/* From net.c. */
int64_t sosu_net_recv(int64_t s, uint8_t *buffer, int64_t size, int32_t *error);
int64_t sosu_net_send(int64_t s, const uint8_t *buffer, int64_t size, int32_t *error);

#define SEND_FAILED (-0x004E) /* MBEDTLS_ERR_NET_SEND_FAILED */
#define RECV_FAILED (-0x004C) /* MBEDTLS_ERR_NET_RECV_FAILED */

/* ---- Once per process: the threads of Mbed TLS, PSA, the roots of the system ---- */

static mbedtls_x509_crt system_roots;
static int system_roots_count = 0;
static int started_ok = 0;

#if defined(_WIN32)
#include <windows.h>
#include <wincrypt.h>

static int alt_mutex_init(mbedtls_platform_mutex_t *m) { InitializeSRWLock(m); return 0; }
static void alt_mutex_destroy(mbedtls_platform_mutex_t *m) { (void)m; }
static int alt_mutex_lock(mbedtls_platform_mutex_t *m) { AcquireSRWLockExclusive(m); return 0; }
static int alt_mutex_unlock(mbedtls_platform_mutex_t *m) { ReleaseSRWLockExclusive(m); return 0; }
static int alt_cond_init(mbedtls_platform_condition_variable_t *c) { InitializeConditionVariable(c); return 0; }
static void alt_cond_destroy(mbedtls_platform_condition_variable_t *c) { (void)c; }
static int alt_cond_signal(mbedtls_platform_condition_variable_t *c) { WakeConditionVariable(c); return 0; }
static int alt_cond_broadcast(mbedtls_platform_condition_variable_t *c) { WakeAllConditionVariable(c); return 0; }
static int alt_cond_wait(mbedtls_platform_condition_variable_t *c, mbedtls_platform_mutex_t *m) {
    return SleepConditionVariableSRW(c, m, INFINITE, 0) ? 0 : -1;
}

/* The certificates of the store ROOT of the user and of the machine. */
static void load_system_roots(void) {
    HCERTSTORE store = CertOpenSystemStoreA(0, "ROOT");
    if (!store) return;
    PCCERT_CONTEXT cert = NULL;
    while ((cert = CertEnumCertificatesInStore(store, cert)) != NULL) {
        if ((cert->dwCertEncodingType & X509_ASN_ENCODING) &&
            mbedtls_x509_crt_parse_der(&system_roots, cert->pbCertEncoded, cert->cbCertEncoded) == 0) {
            system_roots_count++;
        }
    }
    CertCloseStore(store, 0);
}

#elif defined(__APPLE__)
#include <pthread.h>
#include <Security/Security.h>

static void load_system_roots(void) { } /* SecTrust checks the chain: see apple_trusts */

#else
#include <pthread.h>
#include <dirent.h>

/* The bundle of the distribution, or the folders of Android. */
static void load_system_roots(void) {
    static const char *const files[] = {
        "/etc/ssl/certs/ca-certificates.crt", /* Debian, Ubuntu, Arch, Alpine */
        "/etc/pki/tls/certs/ca-bundle.crt",   /* Fedora, RHEL */
        "/etc/ssl/ca-bundle.pem",             /* openSUSE */
        "/etc/pki/tls/cacert.pem",
        "/etc/ssl/cert.pem",
    };
    static const char *const folders[] = {
        "/apex/com.android.conscrypt/cacerts", /* Android 14 and later */
        "/system/etc/security/cacerts",        /* Android */
        "/etc/ssl/certs",
    };
    for (size_t i = 0; i < sizeof files / sizeof *files && system_roots_count == 0; i++) {
        if (mbedtls_x509_crt_parse_file(&system_roots, files[i]) >= 0 && system_roots.raw.p) {
            for (mbedtls_x509_crt *c = &system_roots; c && c->raw.p; c = c->next) system_roots_count++;
        }
    }
    for (size_t i = 0; i < sizeof folders / sizeof *folders && system_roots_count == 0; i++) {
        if (mbedtls_x509_crt_parse_path(&system_roots, folders[i]) >= 0 && system_roots.raw.p) {
            for (mbedtls_x509_crt *c = &system_roots; c && c->raw.p; c = c->next) system_roots_count++;
        }
    }
}
#endif

static void start_once(void) {
#if defined(_WIN32)
    mbedtls_threading_set_alt(alt_mutex_init, alt_mutex_destroy, alt_mutex_lock, alt_mutex_unlock, alt_cond_init,
                              alt_cond_destroy, alt_cond_signal, alt_cond_broadcast, alt_cond_wait);
#endif
    mbedtls_x509_crt_init(&system_roots);
    started_ok = psa_crypto_init() == PSA_SUCCESS;
    if (started_ok) load_system_roots();
}

#if defined(_WIN32)
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK start_callback(PINIT_ONCE o, PVOID p, PVOID *c) {
    (void)o;
    (void)p;
    (void)c;
    start_once();
    return TRUE;
}
static int start(void) {
    InitOnceExecuteOnce(&once, start_callback, NULL, NULL);
    return started_ok;
}
#else
static pthread_once_t once = PTHREAD_ONCE_INIT;
static int start(void) {
    pthread_once(&once, start_once);
    return started_ok;
}
#endif

/* ---- A connection ---- */

typedef struct {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config own_config; /* a client's; a server's is shared, in Server */
    mbedtls_x509_crt roots;        /* those given by the program */
    int has_own_config;
    int64_t socket;
    int32_t net_error; /* the last error of net.c */
} Tls;

typedef struct {
    mbedtls_ssl_config config;
    mbedtls_x509_crt chain;
    mbedtls_pk_context key;
} Server;

static int send_callback(void *context, const unsigned char *buffer, size_t size) {
    Tls *t = context;
    int64_t n = sosu_net_send(t->socket, buffer, (int64_t)size, &t->net_error);
    return n < 0 ? SEND_FAILED : (int)n;
}

static int recv_callback(void *context, unsigned char *buffer, size_t size) {
    Tls *t = context;
    int64_t n = sosu_net_recv(t->socket, buffer, (int64_t)size, &t->net_error);
    return n < 0 ? RECV_FAILED : (int)n;
}

static void free_tls(Tls *t) {
    mbedtls_ssl_free(&t->ssl);
    if (t->has_own_config) mbedtls_ssl_config_free(&t->own_config);
    mbedtls_x509_crt_free(&t->roots);
    free(t);
}

/* What went wrong in a handshake, from its code and the flags of the certificate. */
static int32_t handshake_error(Tls *t, int rc, uint32_t flags) {
    if (rc == SEND_FAILED || rc == RECV_FAILED || rc == MBEDTLS_ERR_SSL_CONN_EOF) return TLS_NETWORK;
    if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED || flags) {
        if (flags & MBEDTLS_X509_BADCERT_CN_MISMATCH) return TLS_WRONG_NAME;
        if (flags & (MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE)) return TLS_EXPIRED;
        return TLS_UNTRUSTED;
    }
    (void)t;
    return TLS_HANDSHAKE;
}

static int32_t run_handshake(Tls *t, int32_t *net_error) {
    int rc;
    do {
        rc = mbedtls_ssl_handshake(&t->ssl);
    } while (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (rc == 0) return TLS_OK;
    int32_t error = handshake_error(t, rc, mbedtls_ssl_get_verify_result(&t->ssl));
    *net_error = error == TLS_NETWORK ? (t->net_error ? t->net_error : 6) : 0;
    return error;
}

/* On Windows and Apple systems, the roots of the system may not all be on the disk (Windows
   fetches those it lacks when it checks a chain), or not readable at all (iOS): when Mbed TLS
   finds no trusted root, the system checks the chain the server sent, for `host`, with its rules
   for TLS servers. Gives a TLS_ code. */
#if defined(_WIN32)
#define SYSTEM_CHECKS 1

static int32_t system_check(Tls *t, const char *host) {
    const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&t->ssl);
    if (!peer) return TLS_UNTRUSTED;
    HCERTSTORE sent = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, NULL);
    if (!sent) return TLS_OTHER;
    PCCERT_CONTEXT leaf = NULL;
    for (const mbedtls_x509_crt *c = peer; c && c->raw.p; c = c->next) {
        CertAddEncodedCertificateToStore(sent, X509_ASN_ENCODING, c->raw.p, (DWORD)c->raw.len, CERT_STORE_ADD_ALWAYS,
                                         c == peer ? &leaf : NULL);
    }
    int32_t result = TLS_UNTRUSTED;
    PCCERT_CHAIN_CONTEXT chain = NULL;
    LPSTR usages[] = { (LPSTR)szOID_PKIX_KP_SERVER_AUTH };
    CERT_CHAIN_PARA para;
    memset(&para, 0, sizeof para);
    para.cbSize = sizeof para;
    para.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
    para.RequestedUsage.Usage.cUsageIdentifier = 1;
    para.RequestedUsage.Usage.rgpszUsageIdentifier = usages;
    if (leaf && CertGetCertificateChain(NULL, leaf, NULL, sent, &para, 0, NULL, &chain)) {
        WCHAR name[256];
        if (MultiByteToWideChar(CP_UTF8, 0, host, -1, name, 256) > 0) {
            SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl;
            memset(&ssl, 0, sizeof ssl);
            ssl.cbSize = sizeof ssl;
            ssl.dwAuthType = AUTHTYPE_SERVER;
            ssl.pwszServerName = name;
            CERT_CHAIN_POLICY_PARA policy;
            memset(&policy, 0, sizeof policy);
            policy.cbSize = sizeof policy;
            policy.pvExtraPolicyPara = &ssl;
            CERT_CHAIN_POLICY_STATUS status;
            memset(&status, 0, sizeof status);
            status.cbSize = sizeof status;
            if (CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policy, &status)) {
                result = status.dwError == 0                       ? TLS_OK
                       : (LONG)status.dwError == CERT_E_EXPIRED    ? TLS_EXPIRED
                       : (LONG)status.dwError == CERT_E_CN_NO_MATCH ? TLS_WRONG_NAME
                                                                    : TLS_UNTRUSTED;
            }
        }
        CertFreeCertificateChain(chain);
    }
    if (leaf) CertFreeCertificateContext(leaf);
    CertCloseStore(sent, 0);
    return result;
}

#elif defined(__APPLE__)
#define SYSTEM_CHECKS 1

static int32_t system_check(Tls *t, const char *host) {
    const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&t->ssl);
    if (!peer) return TLS_UNTRUSTED;
    CFMutableArrayRef certs = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    for (const mbedtls_x509_crt *c = peer; c && c->raw.p; c = c->next) {
        CFDataRef der = CFDataCreate(NULL, c->raw.p, (CFIndex)c->raw.len);
        SecCertificateRef cert = der ? SecCertificateCreateWithData(NULL, der) : NULL;
        if (cert) {
            CFArrayAppendValue(certs, cert);
            CFRelease(cert);
        }
        if (der) CFRelease(der);
    }
    CFStringRef name = CFStringCreateWithCString(NULL, host, kCFStringEncodingUTF8);
    SecPolicyRef policy = SecPolicyCreateSSL(true, name);
    SecTrustRef trust = NULL;
    int trusted = 0;
    if (CFArrayGetCount(certs) > 0 && SecTrustCreateWithCertificates(certs, policy, &trust) == errSecSuccess) {
        trusted = SecTrustEvaluateWithError(trust, NULL) ? 1 : 0;
    }
    if (trust) CFRelease(trust);
    if (policy) CFRelease(policy);
    if (name) CFRelease(name);
    CFRelease(certs);
    return trusted ? TLS_OK : TLS_UNTRUSTED;
}

#else
#define SYSTEM_CHECKS 0
#endif

/* The handshake of a client on `socket`, for `host` (NUL-ended: the name checked in the
   certificate, and sent to the server). The roots are `roots` (PEM, `roots_size` bytes), or
   those of the system when `roots_size` is 0. The connection, or NULL with `*error` set. */
void *sosu_tls_connect(int64_t socket, const uint8_t *host, const uint8_t *roots, int64_t roots_size, int32_t *error,
                       int32_t *net_error) {
    *net_error = 0;
    if (!start()) {
        *error = TLS_OTHER;
        return NULL;
    }
    Tls *t = calloc(1, sizeof *t);
    if (!t) {
        *error = TLS_OTHER;
        return NULL;
    }
    t->socket = socket;
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->own_config);
    mbedtls_x509_crt_init(&t->roots);
    t->has_own_config = 1;
    int system = roots_size == 0;
    if (!system) {
        /* mbedtls_x509_crt_parse wants PEM ended by a NUL, counted in its size */
        unsigned char *pem = malloc((size_t)roots_size + 1);
        int rc = -1;
        if (pem) {
            memcpy(pem, roots, (size_t)roots_size);
            pem[roots_size] = '\0';
            rc = mbedtls_x509_crt_parse(&t->roots, pem, (size_t)roots_size + 1);
            free(pem);
        }
        if (rc != 0) {
            free_tls(t);
            *error = TLS_INVALID;
            return NULL;
        }
    }
    if (mbedtls_ssl_config_defaults(&t->own_config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        free_tls(t);
        *error = TLS_OTHER;
        return NULL;
    }
    mbedtls_ssl_conf_min_tls_version(&t->own_config, MBEDTLS_SSL_VERSION_TLS1_2);
    /* the roots of the system on Windows and Apple systems: decided after the handshake */
    int deferred = system && SYSTEM_CHECKS;
    mbedtls_ssl_conf_authmode(&t->own_config, deferred ? MBEDTLS_SSL_VERIFY_OPTIONAL : MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&t->own_config, system ? &system_roots : &t->roots, NULL);
    if (mbedtls_ssl_setup(&t->ssl, &t->own_config) != 0 ||
        mbedtls_ssl_set_hostname(&t->ssl, (const char *)host) != 0) {
        free_tls(t);
        *error = TLS_OTHER;
        return NULL;
    }
    mbedtls_ssl_set_bio(&t->ssl, t, send_callback, recv_callback, NULL);
    *error = run_handshake(t, net_error);
#if SYSTEM_CHECKS
    uint32_t flags = deferred && *error == TLS_OK ? mbedtls_ssl_get_verify_result(&t->ssl) : 0;
    if (flags & ~(uint32_t)MBEDTLS_X509_BADCERT_NOT_TRUSTED) {
        *error = handshake_error(t, MBEDTLS_ERR_X509_CERT_VERIFY_FAILED, flags); /* a name or a date */
    } else if (flags) {
        *error = system_check(t, (const char *)host); /* no root found: the system decides */
    }
#else
    (void)deferred;
#endif
    if (*error != TLS_OK) {
        free_tls(t);
        return NULL;
    }
    return t;
}

/* What a server answers with: its chain of certificates and its private key, in PEM. NULL with
   `*error` set if they cannot be read. */
void *sosu_tls_server(const uint8_t *chain, int64_t chain_size, const uint8_t *key, int64_t key_size, int32_t *error) {
    if (!start()) {
        *error = TLS_OTHER;
        return NULL;
    }
    Server *s = calloc(1, sizeof *s);
    if (!s) {
        *error = TLS_OTHER;
        return NULL;
    }
    mbedtls_ssl_config_init(&s->config);
    mbedtls_x509_crt_init(&s->chain);
    mbedtls_pk_init(&s->key);
    unsigned char *a = malloc((size_t)chain_size + 1), *b = malloc((size_t)key_size + 1);
    int ok = a && b;
    if (ok) {
        memcpy(a, chain, (size_t)chain_size);
        a[chain_size] = '\0';
        memcpy(b, key, (size_t)key_size);
        b[key_size] = '\0';
        ok = mbedtls_x509_crt_parse(&s->chain, a, (size_t)chain_size + 1) == 0 &&
             mbedtls_pk_parse_key(&s->key, b, (size_t)key_size + 1, NULL, 0) == 0;
        mbedtls_platform_zeroize(b, (size_t)key_size + 1);
    }
    free(a);
    free(b);
    if (!ok) {
        *error = TLS_INVALID;
    } else if (mbedtls_ssl_config_defaults(&s->config, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                           MBEDTLS_SSL_PRESET_DEFAULT) != 0 ||
               mbedtls_ssl_conf_own_cert(&s->config, &s->chain, &s->key) != 0) {
        *error = TLS_OTHER;
        ok = 0;
    }
    if (!ok) {
        mbedtls_ssl_config_free(&s->config);
        mbedtls_x509_crt_free(&s->chain);
        mbedtls_pk_free(&s->key);
        free(s);
        return NULL;
    }
    mbedtls_ssl_conf_min_tls_version(&s->config, MBEDTLS_SSL_VERSION_TLS1_2);
    *error = TLS_OK;
    return s;
}

void sosu_tls_server_free(void *server) {
    Server *s = server;
    mbedtls_ssl_config_free(&s->config);
    mbedtls_x509_crt_free(&s->chain);
    mbedtls_pk_free(&s->key);
    free(s);
}

/* The handshake of the server on `socket`, a connection it accepted. */
void *sosu_tls_accept(void *server, int64_t socket, int32_t *error, int32_t *net_error) {
    *net_error = 0;
    Server *s = server;
    Tls *t = calloc(1, sizeof *t);
    if (!t) {
        *error = TLS_OTHER;
        return NULL;
    }
    t->socket = socket;
    mbedtls_ssl_init(&t->ssl);
    mbedtls_x509_crt_init(&t->roots);
    if (mbedtls_ssl_setup(&t->ssl, &s->config) != 0) {
        free_tls(t);
        *error = TLS_OTHER;
        return NULL;
    }
    mbedtls_ssl_set_bio(&t->ssl, t, send_callback, recv_callback, NULL);
    *error = run_handshake(t, net_error);
    if (*error != TLS_OK) {
        free_tls(t);
        return NULL;
    }
    return t;
}

/* Reads at most `size` bytes: how many (0: the other side closed), or -1 with `*error` set. */
int64_t sosu_tls_read(void *tls, uint8_t *buffer, int64_t size, int32_t *error, int32_t *net_error) {
    Tls *t = tls;
    size_t n = (size_t)(size > 0x40000000 ? 0x40000000 : size);
    for (;;) {
        int rc = mbedtls_ssl_read(&t->ssl, buffer, n);
        if (rc >= 0) {
            *error = TLS_OK;
            return rc;
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE ||
            rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
            continue; /* TLS 1.3 sends tickets after the handshake: nothing to read yet */
        }
        if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rc == MBEDTLS_ERR_SSL_CONN_EOF) {
            *error = TLS_OK;
            return 0;
        }
        *error = rc == RECV_FAILED || rc == SEND_FAILED ? TLS_NETWORK : TLS_HANDSHAKE;
        *net_error = *error == TLS_NETWORK ? t->net_error : 0;
        return -1;
    }
}

/* Writes at most `size` bytes: how many, or -1 with `*error` set. */
int64_t sosu_tls_write(void *tls, const uint8_t *buffer, int64_t size, int32_t *error, int32_t *net_error) {
    Tls *t = tls;
    size_t n = (size_t)(size > 0x40000000 ? 0x40000000 : size);
    for (;;) {
        int rc = mbedtls_ssl_write(&t->ssl, buffer, n);
        if (rc >= 0) {
            *error = TLS_OK;
            return rc;
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        *error = rc == RECV_FAILED || rc == SEND_FAILED ? TLS_NETWORK : TLS_HANDSHAKE;
        *net_error = *error == TLS_NETWORK ? t->net_error : 0;
        return -1;
    }
}

/* The version of TLS the connection uses ("TLSv1.3"), in `out` (32 bytes, NUL-ended). */
void sosu_tls_version(void *tls, uint8_t *out) {
    Tls *t = tls;
    const char *v = mbedtls_ssl_get_version(&t->ssl);
    size_t n = v ? strlen(v) : 0;
    if (n > 31) n = 31;
    memcpy(out, v ? v : "", n);
    out[n] = '\0';
}

/* Says goodbye to the other side (close_notify), then frees the connection; the socket stays
   open, for net.tcp to close. */
void sosu_tls_close(void *tls) {
    Tls *t = tls;
    mbedtls_ssl_close_notify(&t->ssl);
    free_tls(t);
}
