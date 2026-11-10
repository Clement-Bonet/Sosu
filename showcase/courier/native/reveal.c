/* courier: a file shown chosen in Explorer, through the shell of Windows
   (SHOpenFolderAndSelectItems), as browsers do for "Show in folder". The command line of
   explorer.exe is not used: it wants `/select,` outside the quotes that std.os.run puts around
   each argument, and opens its own folder otherwise. Elsewhere, src/files.sosu runs the file
   manager of the system. */
#include <stdint.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <stdlib.h>

/* Shows the file at `path` (NUL-ended UTF-8, `/` or `\`) chosen in its folder: 1, or 0. */
int32_t courier_reveal(const uint8_t *path) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)path, -1, NULL, 0);
    if (n <= 0) return 0;
    wchar_t *wide = malloc((size_t)n * sizeof *wide);
    if (!wide) return 0;
    MultiByteToWideChar(CP_UTF8, 0, (const char *)path, -1, wide, n);
    for (int i = 0; i < n; i++) {
        if (wide[i] == L'/') wide[i] = L'\\';
    }
    HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    PIDLIST_ABSOLUTE item = NULL;
    int32_t shown = 0;
    if (SUCCEEDED(SHParseDisplayName(wide, NULL, &item, 0, NULL)) && item) {
        shown = SUCCEEDED(SHOpenFolderAndSelectItems(item, 0, NULL, 0)) ? 1 : 0;
        CoTaskMemFree(item);
    }
    if (SUCCEEDED(init)) CoUninitialize();
    free(wide);
    return shown;
}

#else
int32_t courier_reveal(const uint8_t *path) {
    (void)path;
    return 0; /* not used: see src/files.sosu */
}
#endif
