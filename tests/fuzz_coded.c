/* The fuzzing of the MP3 and FLAC decoders of the package audio (packages/audio/native/dr.c over
   dr_mp3 and dr_flac, C written elsewhere: THIRD_PARTY_NOTICES.md): any bytes are opened and
   decoded as `audio.mp3` and `audio.flac` do, each in the same 4 MB of memory (`coded.MEMORY`).
   Nothing may crash nor read outside of its memory.

   With libFuzzer (clang -fsanitize=fuzzer): `make fuzz-coded`, which takes dr_libs at the commit of
   packages/audio/sosu.toml. A crash found by the CI is replayed with `build/fuzz_coded crash-...`. */

#include <stdint.h>
#include <stdlib.h>

#include "../packages/audio/native/dr.c"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void decode(int flac, const uint8_t *data, size_t size, uint8_t *memory, int32_t memory_size) {
    static float out[65536];
    int32_t error;
    void *v = flac ? sosu_audio_flac_open(data, (int32_t)size, memory, memory_size, &error)
                   : sosu_audio_mp3_open(data, (int32_t)size, memory, memory_size, &error);
    if (!v) return;
    int32_t channels = flac ? sosu_audio_flac_channels(v) : sosu_audio_mp3_channels(v);
    /* `audio.coded` refuses more than 8 channels before decoding; a bound on the frames keeps each
       input short */
    int32_t total = 0, n = 1;
    while (channels >= 1 && channels <= 8 && total < (1 << 20) && n > 0) {
        n = flac ? sosu_audio_flac_read(v, channels, out, 65536) : sosu_audio_mp3_read(v, channels, out, 65536);
        total += n;
    }
    if (flac) {
        sosu_audio_flac_close(v);
    } else {
        sosu_audio_mp3_close(v);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static uint8_t memory[4194304];
    if (size > INT32_MAX) return 0;
    decode(0, data, size, memory, (int32_t)sizeof memory);
    decode(1, data, size, memory, (int32_t)sizeof memory);
    return 0;
}
