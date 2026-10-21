#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

/* End-to-end tests: `sosuc run` / `sosuc test` on the packages of tests/programs, which
   compiles them through the C compiler, then compares the output with expected.txt.
   Usage (from the repository root): program_tests <path to sosuc> */

typedef struct {
    const char *command; /* with its options */
    const char *program;
    const char *expected; /* file in the program folder */
    int exit_code;
} Case;

static const Case cases[] = {
    { "run", "basics", "expected.txt", 0 },
    { "run", "overflow", "expected.txt", 101 },
    { "run", "classes", "expected.txt", 0 },
    { "run", "linking", "expected.txt", 0 },
    { "run", "files", "expected.txt", 0 },
    { "run", "clock_random", "expected.txt", 0 },
    { "run --release", "overflow", "expected_release.txt", 0 },
    { "test", "testing", "expected.txt", 1 },
};

/* Reads a file in text mode, so that line endings are the same on every system. */
static char *read_text(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) return NULL;
    size_t size = 0, cap = 1024;
    char *text = malloc(cap);
    int c;
    while ((c = fgetc(file)) != EOF) {
        if (size + 1 >= cap) text = realloc(text, cap *= 2);
        text[size++] = (char)c;
    }
    text[size] = '\0';
    fclose(file);
    return text;
}

static int run(const char *command) {
    fflush(stdout);
    int status = system(command);
#ifndef _WIN32
    if (status != -1) status = (status >> 8) & 0xFF;
#endif
    return status;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fputs("usage: program_tests <path to sosuc>\n", stderr);
        return 2;
    }
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        const Case *c = &cases[i];
#ifdef _WIN32
        const char *sep = "\\";
#else
        const char *sep = "/";
#endif
        char command[512], output[256], expected_path[256];
        snprintf(output, sizeof output, "build%s%s.out", sep, c->program);
        snprintf(command, sizeof command, "%s %s tests/programs/%s > %s 2> build%s%s.err", argv[1], c->command,
                 c->program, output, sep, c->program);
        int status = run(command);
        CHECK(status == c->exit_code, "%s %s: exit code %d, expected %d (see build/%s.err)", c->command, c->program,
              status, c->exit_code, c->program);

        snprintf(expected_path, sizeof expected_path, "tests/programs/%s/%s", c->program, c->expected);
        char *actual = read_text(output);
        char *expected = read_text(expected_path);
        CHECK(actual && expected && strcmp(actual, expected) == 0, "%s %s: output differs\n--- got:\n%s--- expected:\n%s",
              c->command, c->program, actual ? actual : "(none)\n", expected ? expected : "(none)\n");
        free(actual);
        free(expected);
    }

    /* `copy` of the [link] section of tests/programs/linking */
    char *copied = read_text("tests/programs/linking/build/message.txt");
    CHECK(copied && strstr(copied, "Copied next to the program"), "linking: message.txt was not copied to build/");
    free(copied);
    return test_summary("program tests");
}
