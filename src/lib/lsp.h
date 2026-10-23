#ifndef SOSU_LSP_H
#define SOSU_LSP_H

#include "common.h"

/* The language server of `sosuc lsp` (spec §11.4): the Language Server Protocol, on the
   checks of the compiler. Diagnostics while typing, hover with the types, go to
   definition, completion. This part only handles messages; `sosuc lsp` reads them from
   stdin and writes the answers to stdout, each behind a `Content-Length` header. */

typedef struct LspServer LspServer;

/* JSON texts to send to the client, in order. */
typedef struct {
    char **items;
    size_t count;
    size_t cap;
} LspMessages;

/* A server whose `core` and `std` packages are in `lib_dir`. */
LspServer *lsp_new(const char *lib_dir);
void lsp_free(LspServer *server);

/* Handles one message of the client (a JSON-RPC text, without its header), and appends
   the answers and notifications to send to `out`. Returns false once the client asked
   the server to exit: lsp_exit_code then gives the exit status. */
bool lsp_handle(LspServer *server, const char *text, size_t length, LspMessages *out);
int lsp_exit_code(const LspServer *server);
void lsp_messages_free(LspMessages *messages);

/* The normalized path of a `file://` URI (NULL for another scheme), and back. New strings. */
char *lsp_uri_to_path(const char *uri);
char *lsp_path_to_uri(const char *path);

#endif
