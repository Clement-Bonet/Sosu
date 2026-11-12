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
	@# the readers of models, fuzzed by `sosuc fuzz`: their seeds and the inputs that once failed
	./build/sosuc fuzz --replay tests/fuzz_sosu/gltf -- tests/fuzz_sosu/gltf/corpus > /dev/null
	./build/sosuc fuzz --replay tests/fuzz_sosu/obj -- tests/fuzz_sosu/obj/corpus > /dev/null
	./build/sosuc fuzz --replay tests/fuzz_sosu/fbx -- tests/fuzz_sosu/fbx/corpus > /dev/null
	./build/sosuc fuzz --replay tests/fuzz_sosu/sofa -- tests/fuzz_sosu/sofa/corpus > /dev/null
	./build/sosuc check tests/packages/app
	./build/sosuc check packages/platform
	./build/sosuc test packages/platform
	./build/sosuc check examples/window
	./build/sosuc check tests/window
	./build/sosuc test packages/math
	./build/sosuc test packages/model
	./build/sosuc test packages/scene
	./build/sosuc test packages/anim
	./build/sosuc test packages/particles
	./build/sosuc check packages/assets
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
	./build/sosuc test packages/audio
	./build/sosuc check examples/sound
	./build/sosuc check examples/dialogs
	@# 3D (spec 8.2): built to the end, through the C compiler, without opening its window
	./build/sosuc check packages/gpu
	./build/sosuc build examples/cube
	./build/sosuc build examples/pads
	./build/sosuc check packages/render3d
	./build/sosuc build examples/viewer
	@# its tests open a player: SDL's dummy driver, for the machines without a sound card (the CI)
	SDL_AUDIO_DRIVER=dummy ./build/sosuc test showcase/beatbox
	./build/sosuc test showcase/disktiles
	./build/sosuc test showcase/notes
	./build/sosuc test showcase/pixelforge
	./build/sosuc test showcase/queens
	./build/sosuc test showcase/sheets
	./build/sosuc test showcase/vault
	./build/sosuc test showcase/courier
	./build/sosuc test showcase/audioplayer
	./build/sosuc test showcase/soundstage
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

# The Ogg Vorbis decoder of the package audio (tests/fuzz_ogg.c, stb_vorbis), from the Ogg files
# of tests/programs/sounds: its inputs kept in build/fuzz_ogg_corpus, a crash in build/fuzz_crashes/.
fuzz-ogg: build/fuzz_ogg
	mkdir -p build/fuzz_ogg_corpus build/fuzz_crashes
	./build/fuzz_ogg -max_total_time=$(FUZZ_TIME) -max_len=65536 -timeout=10 -rss_limit_mb=2048 -detect_leaks=0 \
		-print_final_stats=1 -artifact_prefix=build/fuzz_crashes/ogg- build/fuzz_ogg_corpus tests/programs/sounds/tests/data

build/fuzz_ogg: packages/audio/native/ogg.c packages/audio/native/vendor/stb_vorbis.c tests/fuzz_ogg.c | build
	$(FUZZ_CC) -std=c99 -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -o $@ tests/fuzz_ogg.c -lm

# The MP3 and FLAC decoders of the package audio (tests/fuzz_coded.c, dr_mp3 and dr_flac), from the
# files of tests/programs/sounds. dr_libs is taken at the commit of packages/audio/sosu.toml, without
# its history, into build/dr_libs. dr_flac adds and multiplies samples of a hostile file in int
# without checks: -fwrapv makes that wrap (wrong samples, nothing else), and the check of signed
# overflow is left out; the other checks of UBSan, and ASan, stay.
DR_LIBS_REV := $(shell sed -n 's/^rev = "\(.*\)"/\1/p' packages/audio/sosu.toml)

fuzz-coded: build/fuzz_coded
	mkdir -p build/fuzz_coded_corpus build/fuzz_crashes
	./build/fuzz_coded -max_total_time=$(FUZZ_TIME) -max_len=65536 -timeout=10 -rss_limit_mb=2048 -detect_leaks=0 \
		-print_final_stats=1 -artifact_prefix=build/fuzz_crashes/coded- build/fuzz_coded_corpus tests/programs/sounds/tests/data

build/dr_libs/$(DR_LIBS_REV): | build
	rm -rf build/dr_libs
	mkdir -p build/dr_libs/src
	cd build/dr_libs/src && git init -q && git fetch -q --depth 1 https://github.com/mackron/dr_libs $(DR_LIBS_REV) \
		&& git checkout -q FETCH_HEAD
	touch $@

build/fuzz_coded: packages/audio/native/dr.c tests/fuzz_coded.c build/dr_libs/$(DR_LIBS_REV) | build
	$(FUZZ_CC) -std=c99 -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -fwrapv \
		-fno-sanitize=signed-integer-overflow -Ibuild/dr_libs/src -o $@ tests/fuzz_coded.c -lm

# sosu.toml (tests/fuzz_manifest.c), from every sosu.toml of the repository.
fuzz-manifest: build/fuzz_manifest
	mkdir -p build/fuzz_manifest_corpus build/fuzz_manifest_seeds build/fuzz_crashes
	n=0; for f in $$(git ls-files '*sosu.toml'); do n=$$((n + 1)); cp "$$f" build/fuzz_manifest_seeds/$$n.toml; done
	./build/fuzz_manifest -max_total_time=$(FUZZ_TIME) -max_len=4096 -timeout=10 -rss_limit_mb=2048 -detect_leaks=0 \
		-print_final_stats=1 -artifact_prefix=build/fuzz_crashes/manifest- build/fuzz_manifest_corpus build/fuzz_manifest_seeds

build/fuzz_manifest: $(LIB_SRC) $(LIB_HDR) tests/fuzz_manifest.c | build
	$(FUZZ_CC) $(CPPFLAGS) -std=c99 -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
		-o $@ $(LIB_SRC) tests/fuzz_manifest.c

# The interface files (tests/fuzz_ui.c), from every .ui.sosu file of the repository.
fuzz-ui: build/fuzz_ui
	mkdir -p build/fuzz_ui_corpus build/fuzz_ui_seeds build/fuzz_crashes
	n=0; for f in $$(git ls-files '*.ui.sosu'); do n=$$((n + 1)); cp "$$f" build/fuzz_ui_seeds/$$n.ui.sosu; done
	./build/fuzz_ui -max_total_time=$(FUZZ_TIME) -max_len=16384 -timeout=10 -rss_limit_mb=2048 -detect_leaks=0 \
		-dict=tests/fuzz.dict -print_final_stats=1 -artifact_prefix=build/fuzz_crashes/ui- build/fuzz_ui_corpus build/fuzz_ui_seeds

build/fuzz_ui: $(LIB_SRC) $(LIB_HDR) tests/fuzz_ui.c | build
	$(FUZZ_CC) $(CPPFLAGS) -std=c99 -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
		-o $@ $(LIB_SRC) tests/fuzz_ui.c

# sosuc bindgen (tests/fuzz_bindgen.c), from the C headers of the repository, read as if preprocessed,
# and the inputs that crashed it once (tests/fuzz_bindgen/).
fuzz-bindgen: build/fuzz_bindgen
	mkdir -p build/fuzz_bindgen_corpus build/fuzz_bindgen_seeds build/fuzz_crashes
	cp tests/programs/bindgen/c/*.h tests/fuzz_bindgen/* src/lib/*.h lib/runtime/sosu_rt.h build/fuzz_bindgen_seeds/
	./build/fuzz_bindgen -max_total_time=$(FUZZ_TIME) -max_len=16384 -timeout=10 -rss_limit_mb=2048 -detect_leaks=0 \
		-print_final_stats=1 -artifact_prefix=build/fuzz_crashes/bindgen- build/fuzz_bindgen_corpus build/fuzz_bindgen_seeds

build/fuzz_bindgen: $(LIB_SRC) $(LIB_HDR) tests/fuzz_bindgen.c | build
	$(FUZZ_CC) $(CPPFLAGS) -std=c99 -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
		-o $@ $(LIB_SRC) tests/fuzz_bindgen.c

# The language server (tests/fuzz_lsp.c): messages cut at each NUL byte, from the sessions of tests/fuzz_lsp.
fuzz-lsp: build/fuzz_lsp
	mkdir -p build/fuzz_lsp_corpus build/fuzz_crashes
	./build/fuzz_lsp -max_total_time=$(FUZZ_TIME) -max_len=16384 -timeout=10 -rss_limit_mb=2048 -detect_leaks=0 \
		-dict=tests/fuzz.dict -print_final_stats=1 -artifact_prefix=build/fuzz_crashes/lsp- build/fuzz_lsp_corpus tests/fuzz_lsp

build/fuzz_lsp: $(LIB_SRC) $(LIB_HDR) tests/fuzz_lsp.c | build
	$(FUZZ_CC) $(CPPFLAGS) -std=c99 -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
		-o $@ $(LIB_SRC) tests/fuzz_lsp.c

# The readers of models of the package model, code of Sosu fuzzed by `sosuc fuzz` (spec 13.1,
# work 5): glTF, OBJ, FBX and SOFA (package audio), FUZZ_TIME seconds each, from the seeds of tests/fuzz_sosu/<name>/corpus;
# their inputs kept in build/fuzz_<name>_corpus, a crash in build/fuzz_crashes/<name>-.
fuzz-models: build/sosuc
	mkdir -p build/fuzz_gltf_corpus build/fuzz_obj_corpus build/fuzz_fbx_corpus build/fuzz_sofa_corpus build/fuzz_crashes
	for t in gltf obj fbx sofa; do \
		SOSU_CC=$(FUZZ_CC) ./build/sosuc fuzz tests/fuzz_sosu/$$t -- -max_total_time=$(FUZZ_TIME) -max_len=8192 -timeout=10 \
			-rss_limit_mb=2048 -detect_leaks=0 -print_final_stats=1 -artifact_prefix=build/fuzz_crashes/$$t- \
			build/fuzz_$${t}_corpus tests/fuzz_sosu/$$t/corpus || exit 1; \
	done

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
DOC_PACKAGES := audio compress crypto db image math net sqlite testing
DOC_SOURCES := $(sort $(wildcard lib/core/src/*.sosu lib/std/src/*.sosu) packages/platform/src/dialogs.sosu \
	$(filter-out %/lib.sosu,$(foreach p,$(DOC_PACKAGES),$(wildcard packages/$(p)/src/*.sosu))))

doc: build/docgen
	./build/docgen docs/REFERENCE.md $(DOC_SOURCES)

build/docgen: src/docgen/docgen.c src/lib/docgen.c src/lib/docgen.h src/lib/common.c src/lib/common.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ src/docgen/docgen.c src/lib/docgen.c src/lib/common.c

clean:
	rm -rf build

.PHONY: all test clean playground doc fuzz-ogg fuzz-coded fuzz-manifest fuzz-ui fuzz-bindgen fuzz-lsp fuzz-models
