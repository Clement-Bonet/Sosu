# Builds sosuc with gcc or clang. Usage: make [test]
CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -pedantic -Werror -g
CPPFLAGS += -Isrc/lib

LIB_SRC := $(wildcard src/lib/*.c)
LIB_HDR := $(wildcard src/lib/*.h src/lib/*.inc)
TESTS := build/lexer_tests build/parser_tests build/package_tests build/sema_tests build/fmt_tests build/json_tests build/lsp_tests build/i18n_tests build/program_tests build/fuzz_replay

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
	./build/fuzz_replay tests/fuzz/* > /dev/null
	./build/sosuc check tests/packages/app
	./build/sosuc check packages/platform
	./build/sosuc test packages/platform
	./build/sosuc check examples/window
	./build/sosuc check tests/window
	./build/sosuc test packages/image
	./build/sosuc test packages/render
	./build/sosuc test packages/ui
	./build/sosuc test packages/i18n
	./build/sosuc check examples/form
	./build/sosuc check examples/editor
	./build/sosuc check examples/workspace
	./build/sosuc check examples/themes
	./build/sosuc check examples/containers
	./build/sosuc check packages/audio
	./build/sosuc check examples/sound
	./build/sosuc check examples/dialogs
	@for f in tests/samples/*.sosu; do ./build/sosuc parse "$$f" > /dev/null || { echo "FAIL $$f"; exit 1; }; done
	./build/sosuc parse tests/samples/ui/login.ui.sosu > /dev/null
	@for f in tests/samples/ui/*.sosu; do ./build/sosuc lex "$$f" > /dev/null || { echo "FAIL $$f"; exit 1; }; done
	@echo "samples: ok"
	@# the error codes are frozen (spec 13.2): each one given is in docs/errors.md, and one no longer
	@# given stays there, marked as such, so that it is never given again for something else
	@grep -ohE '"E[0-9]{4}"' src/lib/*.c src/sosuc/main.c | tr -d '"' | sort -u > build/codes.txt
	@for c in $$(cat build/codes.txt); do grep -q "^| $$c |" docs/errors.md || { echo "FAIL $$c is not in docs/errors.md"; exit 1; }; done
	@for c in $$(grep -oE '^\| E[0-9]{4}' docs/errors.md | tr -d '| '); do grep -qx "$$c" build/codes.txt || \
		grep "^| $$c |" docs/errors.md | grep -q "not given any more" || \
		{ echo "FAIL $$c is in docs/errors.md but not given, nor marked as not given any more"; exit 1; }; done
	@echo "error codes: ok"

# The time of sosuc on large programs (tests/bench.c), optimized: fails if it grows faster than
# the program. `build/bench <modules> <blocks>` measures one size.
bench: build/bench
	./build/bench

build/bench: $(LIB_SRC) $(LIB_HDR) tests/bench.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -O2 -o $@ $(LIB_SRC) tests/bench.c

# The fuzzing of the parser, the semantic analysis and code generation (tests/fuzz.c), by
# libFuzzer, with clang: FUZZ_TIME seconds, the inputs found kept in build/fuzz_corpus, a crash
# written to build/fuzz_crashes/. build/fuzz_replay runs files once each, without libFuzzer.
FUZZ_CC ?= clang
FUZZ_TIME ?= 60
FUZZ_SEEDS := tests/samples tests/fuzz lib/core/src lib/std/src

fuzz: build/fuzz
	mkdir -p build/fuzz_corpus build/fuzz_crashes
	./build/fuzz -max_total_time=$(FUZZ_TIME) -max_len=8192 -timeout=10 -rss_limit_mb=2048 -detect_leaks=0 \
		-dict=tests/fuzz.dict -print_final_stats=1 -artifact_prefix=build/fuzz_crashes/ build/fuzz_corpus $(FUZZ_SEEDS)

build/fuzz: $(LIB_SRC) $(LIB_HDR) tests/fuzz.c | build
	$(FUZZ_CC) $(CPPFLAGS) -std=c99 -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
		-o $@ $(LIB_SRC) tests/fuzz.c

build/fuzz_replay: $(LIB_SRC) $(LIB_HDR) tests/fuzz.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -DSOSU_FUZZ_MAIN -o $@ $(LIB_SRC) tests/fuzz.c

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

# The reference of the public API of `core`, `std` and the packages of spec §8.3, from their
# comments (src/docgen); the lib.sosu of a package only imports its modules. Committed; the CI
# checks that it is up to date.
DOC_PACKAGES := audio compress crypto db image math sqlite testing
DOC_SOURCES := $(sort $(wildcard lib/core/src/*.sosu lib/std/src/*.sosu) packages/platform/src/dialogs.sosu \
	$(filter-out %/lib.sosu,$(foreach p,$(DOC_PACKAGES),$(wildcard packages/$(p)/src/*.sosu))))

doc: build/docgen
	./build/docgen docs/REFERENCE.md $(DOC_SOURCES)

build/docgen: src/docgen/docgen.c | build
	$(CC) $(CFLAGS) -o $@ src/docgen/docgen.c

clean:
	rm -rf build

.PHONY: all test clean playground doc
