/* audio.ogg: the decoder of stb_vorbis (vendor/stb_vorbis.c, public domain, see
   THIRD_PARTY_NOTICES.md) behind five functions. Every allocation of stb_vorbis comes from
   `memory`, given by Sosu: a hostile header cannot ask for more, and nothing is freed. */
#include <stdint.h>

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_COMMENTS /* never read by Sosu: the comment header is skipped unread */
#include "vendor/stb_vorbis.c"

/* The decoder of the Ogg Vorbis file of `size` bytes at `data`, or NULL with `*error` set:
   1 when `memory` is too small, 2 when the file is damaged or not Ogg Vorbis. */
void *sosu_audio_ogg_open(const uint8_t *data, int32_t size, uint8_t *memory, int32_t memory_size,
                          int32_t *error) {
    stb_vorbis_alloc alloc;
    int code = 0;
    alloc.alloc_buffer = (char *)memory;
    alloc.alloc_buffer_length_in_bytes = memory_size;
    stb_vorbis *v = stb_vorbis_open_memory(data, size, &code, &alloc);
    *error = v ? 0 : code == VORBIS_outofmem ? 1 : 2;
    return v;
}

int32_t sosu_audio_ogg_channels(void *v) {
    return stb_vorbis_get_info((stb_vorbis *)v).channels;
}

int32_t sosu_audio_ogg_rate(void *v) {
    return (int32_t)stb_vorbis_get_info((stb_vorbis *)v).sample_rate;
}

/* Decodes up to `floats / channels` frames into `out`, interleaved; gives the frames decoded,
   0 at the end of the stream (or when it is damaged further on). */
int32_t sosu_audio_ogg_read(void *v, int32_t channels, float *out, int32_t floats) {
    return stb_vorbis_get_samples_float_interleaved((stb_vorbis *)v, channels, out, floats);
}

void sosu_audio_ogg_close(void *v) {
    stb_vorbis_close((stb_vorbis *)v);
}
