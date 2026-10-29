# Builds sosuc with gcc or clang. Usage: make [test]
CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -pedantic -Werror -g
CPPFLAGS += -Isrc/lib

LIB_SRC := $(wildcard src/lib/*.c)
LIB_HDR := $(wildcard src/lib/*.h)
TESTS := build/lexer_tests build/parser_tests build/package_tests build/sema_tests build/fmt_tests build/json_tests build/lsp_tests build/i18n_tests build/program_tests

all: build/sosuc $(TESTS)

build:
	mkdir -p build

build/sosuc: $(LIB_SRC) $(LIB_HDR) src/sosuc/main.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(LIB_SRC) src/sosuc/main.c

build/%_tests: $(LIB_SRC) $(LIB_HDR) tests/%_tests.c tests/test.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(LIB_SRC) tests/$*_tests.c

test: all
	./build/lexer_tests
	./build/parser_tests
	./build/package_tests
	./build/sema_tests
	./build/fmt_tests
	./build/json_tests
	./build/lsp_tests
	./build/i18n_tests
	./build/program_tests ./build/sosuc
	./build/sosuc check tests/packages/app
	./build/sosuc check packages/platform
	./build/sosuc check examples/window
	./build/sosuc check tests/window
	./build/sosuc test packages/render
	./build/sosuc test packages/ui
	./build/sosuc test packages/i18n
	./build/sosuc check examples/form
	./build/sosuc check examples/editor
	./build/sosuc check examples/workspace
	./build/sosuc check examples/themes
	./build/sosuc check examples/containers
	@for f in tests/samples/*.sosu; do ./build/sosuc parse "$$f" > /dev/null || { echo "FAIL $$f"; exit 1; }; done
	./build/sosuc parse tests/samples/ui/login.ui.sosu > /dev/null
	@for f in tests/samples/ui/*.sosu; do ./build/sosuc lex "$$f" > /dev/null || { echo "FAIL $$f"; exit 1; }; done
	@echo "samples: ok"

# The compiler in a web page, with Emscripten: playground/ and the module it loads, the library
# embedded at /lib. The compiler recurses on the syntax trees: the default stack (64 KB) is too
# small. Published by .github/workflows/playground.yml.
EMCC ?= emcc
PLAYGROUND_FLAGS := -sMODULARIZE=1 -sEXPORT_NAME=createSosu -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=8MB \
	-sEXPORTED_FUNCTIONS=_sosu_playground_compile,_sosu_playground_format,_free -sEXPORTED_RUNTIME_METHODS=ccall,UTF8ToString

playground: build/playground/sosuc.js

build/playground/sosuc.js: $(LIB_SRC) $(LIB_HDR) src/playground/playground.c playground/index.html playground/playground.js $(shell find lib -type f)
	mkdir -p build/playground
	$(EMCC) $(CPPFLAGS) -std=c99 -Wall -O2 $(PLAYGROUND_FLAGS) --embed-file lib@/lib -o $@ $(LIB_SRC) src/playground/playground.c
	cp playground/index.html playground/playground.js build/playground/

clean:
	rm -rf build

.PHONY: all test clean playground
