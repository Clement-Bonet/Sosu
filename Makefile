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

clean:
	rm -rf build

.PHONY: all test clean
