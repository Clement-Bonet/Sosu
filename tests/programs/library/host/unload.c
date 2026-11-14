/* The library `geom` loaded while the host runs, made to fault (its guard catches it), unloaded,
   then a fault of the host itself, which the host catches: the handlers of faults of the library
   must have left with it, or this last fault would run code that is gone (spec §9.2). */
#include <stddef.h>
#include <stdio.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <setjmp.h>
#include <signal.h>
#include <string.h>
static sigjmp_buf back;
static void host_fault(int sig) {
    (void)sig;
    siglongjmp(back, 1);
}
#endif

typedef int (*poke_fn)(size_t);
typedef const char *(*error_fn)(void);

int main(int argc, char **argv) {
    if (argc != 2) return 2;
#ifndef _WIN32
    struct sigaction host;
    memset(&host, 0, sizeof host);
    host.sa_handler = host_fault;
    sigemptyset(&host.sa_mask);
    sigaction(SIGSEGV, &host, NULL); /* the host's own, before the library comes */
    sigaction(SIGBUS, &host, NULL);
    void *lib = dlopen(argv[1], RTLD_NOW);
    if (!lib) return 3;
    poke_fn poke = (poke_fn)dlsym(lib, "geom_poke");
    error_fn error = (error_fn)dlsym(lib, "geom_error");
#else
    HMODULE lib = LoadLibraryA(argv[1]);
    if (!lib) return 3;
    poke_fn poke = (poke_fn)(void (*)(void))GetProcAddress(lib, "geom_poke");
    error_fn error = (error_fn)(void (*)(void))GetProcAddress(lib, "geom_error");
#endif
    if (!poke || !error) return 4;
    int poked = poke(16);
    printf("poke %d, error: %s\n", poked, error() ? "caught by the library" : "none");
#ifdef _WIN32
    FreeLibrary(lib);
    int caught = 0;
    __try {
        volatile int *p = (volatile int *)16;
        *p = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        caught = 1;
    }
#else
    dlclose(lib);
    volatile int caught = 0;
    if (sigsetjmp(back, 1) == 0) {
        volatile int *p = (volatile int *)16;
        *p = 1;
    } else {
        caught = 1;
    }
#endif
    printf("after it is unloaded, a fault of the host: %s\n", caught ? "caught by the host" : "missed");
    return 0;
}
