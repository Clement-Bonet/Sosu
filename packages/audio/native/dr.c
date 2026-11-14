/* audio.mp3 and audio.flac: the decoders of dr_mp3 and dr_flac (dr_libs, public domain or MIT No
   Attribution, see THIRD_PARTY_NOTICES.md), taken from their repository by [native.dr_libs], behind
   five functions each. Every allocation of a decoder comes from `memory`, given by Sosu, a pool that
   only grows: a hostile file cannot ask for more, and nothing is freed before the pool is. */
#include <stdint.h>
#include <string.h>

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "dr_mp3.h"

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG /* FLAC in Ogg: not read */
#include "dr_flac.h"

/* At the start of `memory`: where the next block goes, and whether a block was refused. Each block
   has 16 bytes before it, its size in the first 8. */
typedef struct {
    uint8_t *base;
    size_t size;
    size_t used;
    size_t last; /* the offset of the last block, to grow it in place */
    int refused;
} Pool;

static Pool *pool_of(uint8_t *memory, int32_t memory_size) {
    uintptr_t at = ((uintptr_t)memory + 15) & ~(uintptr_t)15;
    size_t skip = (size_t)(at - (uintptr_t)memory);
    size_t head = (sizeof(Pool) + 15) & ~(size_t)15;
    if (memory_size < 0 || (size_t)memory_size < skip + head + 64) return NULL;
    Pool *p = (Pool *)at;
    p->base = (uint8_t *)at + head;
    p->size = (size_t)memory_size - skip - head;
    p->used = 0;
    p->last = (size_t)-1;
    p->refused = 0;
    return p;
}

static void *pool_malloc(size_t size, void *user) {
    Pool *p = (Pool *)user;
    size_t rounded = (size + 15) & ~(size_t)15;
    if (rounded < size || p->size - p->used < 16 || p->size - p->used - 16 < rounded) {
        p->refused = 1;
        return NULL;
    }
    uint8_t *block = p->base + p->used;
    memcpy(block, &size, sizeof size);
    p->last = p->used;
    p->used += 16 + rounded;
    return block + 16;
}

static void *pool_realloc(void *old, size_t size, void *user) {
    Pool *p = (Pool *)user;
    if (!old) return pool_malloc(size, user);
    uint8_t *block = (uint8_t *)old - 16;
    size_t had;
    memcpy(&had, block, sizeof had);
    if (size <= had) return old;
    size_t offset = (size_t)(block - p->base);
    if (offset == p->last) {
        /* the last block grows where it is */
        size_t rounded = (size + 15) & ~(size_t)15;
        if (rounded < size || p->size - offset - 16 < rounded) {
            p->refused = 1;
            return NULL;
        }
        memcpy(block, &size, sizeof size);
        p->used = offset + 16 + rounded;
        return old;
    }
    void *grown = pool_malloc(size, user);
    if (grown) memcpy(grown, old, had);
    return grown;
}

static void pool_free(void *block, void *user) {
    (void)block;
    (void)user; /* the pool is freed whole, by Sosu */
}

/* ---- MP3 ---- */

/* The decoder of the MP3 file of `size` bytes at `data`, or NULL with `*error` set: 1 when `memory`
   is too small, 2 when the file is damaged or not MP3. */
void *sosu_audio_mp3_open(const uint8_t *data, int32_t size, uint8_t *memory, int32_t memory_size, int32_t *error) {
    Pool *p = pool_of(memory, memory_size);
    if (!p) {
        *error = 1;
        return NULL;
    }
    drmp3_allocation_callbacks callbacks = { p, pool_malloc, pool_realloc, pool_free };
    drmp3 *mp3 = (drmp3 *)pool_malloc(sizeof(drmp3), p);
    if (!mp3) {
        *error = 1;
        return NULL;
    }
    if (!drmp3_init_memory(mp3, data, (size_t)size, &callbacks)) {
        *error = p->refused ? 1 : 2;
        return NULL;
    }
    *error = 0;
    return mp3;
}

int32_t sosu_audio_mp3_channels(void *v) {
    return (int32_t)((drmp3 *)v)->channels;
}

int32_t sosu_audio_mp3_rate(void *v) {
    return (int32_t)((drmp3 *)v)->sampleRate;
}

/* Decodes up to `floats / channels` frames into `out`, interleaved; gives the frames decoded, 0 at
   the end (or where the file is damaged further on). */
int32_t sosu_audio_mp3_read(void *v, int32_t channels, float *out, int32_t floats) {
    if (channels <= 0 || floats < channels) return 0;
    return (int32_t)drmp3_read_pcm_frames_f32((drmp3 *)v, (drmp3_uint64)(floats / channels), out);
}

/* Back to the start, for a sound that loops (audio.stream): 1 when it could. */
int32_t sosu_audio_mp3_rewind(void *v) {
    return drmp3_seek_to_pcm_frame((drmp3 *)v, 0) ? 1 : 0;
}

/* The frames of the file (0 when unknown: dr_mp3 reads the headers of every frame to count them,
   then comes back to where it was), and a frame to go to (1 when it could). */
int64_t sosu_audio_mp3_length(void *v) {
    return (int64_t)drmp3_get_pcm_frame_count((drmp3 *)v);
}

int32_t sosu_audio_mp3_seek(void *v, int64_t frame) {
    return frame >= 0 && drmp3_seek_to_pcm_frame((drmp3 *)v, (drmp3_uint64)frame) ? 1 : 0;
}

void sosu_audio_mp3_close(void *v) {
    drmp3_uninit((drmp3 *)v);
}

/* ---- FLAC ---- */

/* The decoder of the FLAC file of `size` bytes at `data`, as sosu_audio_mp3_open. */
void *sosu_audio_flac_open(const uint8_t *data, int32_t size, uint8_t *memory, int32_t memory_size, int32_t *error) {
    Pool *p = pool_of(memory, memory_size);
    if (!p) {
        *error = 1;
        return NULL;
    }
    drflac_allocation_callbacks callbacks = { p, pool_malloc, pool_realloc, pool_free };
    drflac *flac = drflac_open_memory(data, (size_t)size, &callbacks);
    if (!flac) {
        *error = p->refused ? 1 : 2;
        return NULL;
    }
    *error = 0;
    return flac;
}

int32_t sosu_audio_flac_channels(void *v) {
    return (int32_t)((drflac *)v)->channels;
}

int32_t sosu_audio_flac_rate(void *v) {
    return (int32_t)((drflac *)v)->sampleRate;
}

int32_t sosu_audio_flac_read(void *v, int32_t channels, float *out, int32_t floats) {
    if (channels <= 0 || floats < channels) return 0;
    return (int32_t)drflac_read_pcm_frames_f32((drflac *)v, (drflac_uint64)(floats / channels), out);
}

int32_t sosu_audio_flac_rewind(void *v) {
    return drflac_seek_to_pcm_frame((drflac *)v, 0) ? 1 : 0;
}

int64_t sosu_audio_flac_length(void *v) {
    drflac_uint64 n = ((drflac *)v)->totalPCMFrameCount;
    return n > (drflac_uint64)INT64_MAX ? 0 : (int64_t)n;
}

int32_t sosu_audio_flac_seek(void *v, int64_t frame) {
    return frame >= 0 && drflac_seek_to_pcm_frame((drflac *)v, (drflac_uint64)frame) ? 1 : 0;
}

void sosu_audio_flac_close(void *v) {
    drflac_close((drflac *)v);
}
