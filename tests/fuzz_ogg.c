/* The fuzzing of the Ogg Vorbis decoder of the package audio (packages/audio/native/ogg.c, C
   written elsewhere: THIRD_PARTY_NOTICES.md): any bytes are opened and decoded as `audio.ogg`
   does, in the same 4 MB of memory (`MEMORY`). Nothing may crash nor read outside of its memory.

   With libFuzzer (clang -fsanitize=fuzzer): `make fuzz-ogg`. A crash found by the CI is replayed
   with `build/fuzz_ogg crash-...`. */

#include <stdint.h>
#include <stdlib.h>

#include "../packages/audio/native/ogg.c"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static uint8_t memory[4194304];
    static float out[65536];
    if (size > INT32_MAX) return 0;
    int32_t error;
    void *v = sosu_audio_ogg_open(data, (int32_t)size, memory, (int32_t)sizeof memory, &error);
    if (!v) return 0;
    int32_t channels = sosu_audio_ogg_channels(v);
    /* `audio.ogg` refuses more than 8 channels before decoding; a bound on the frames keeps each
       input short */
    int32_t total = 0, n = 1;
    while (channels >= 1 && channels <= 8 && total < (1 << 20) && n > 0) {
        n = sosu_audio_ogg_read(v, channels, out, 65536);
        total += n;
    }
    sosu_audio_ogg_close(v);
    return 0;
}
