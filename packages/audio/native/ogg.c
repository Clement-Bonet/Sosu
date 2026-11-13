/* audio.ogg: the decoder of stb_vorbis (vendor/stb_vorbis.c, public domain, see
   THIRD_PARTY_NOTICES.md) behind a few functions. Every allocation of stb_vorbis comes from
   `memory`, given by Sosu: a hostile header cannot ask for more, and nothing is freed. */
#include <stdint.h>
#include <string.h>

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_COMMENTS /* never read by Sosu: the comment header is skipped unread */
#include "vendor/stb_vorbis.c"

/* A chained file holds logical streams one after the other (a radio recorded, tracks joined).
   stb_vorbis reads one, so the file is cut here into its links, each starting at a page that begins
   a stream (BOS) after the audio pages of another, and each link is opened in turn into the same
   memory. The links after the first must have its channels and its rate (the ring of audio.stream
   keeps them): the sound ends at the first that does not. */
#define SOSU_OGG_LINKS 256

typedef struct {
    uint32_t offset, size;
    uint32_t frames; /* the granule position of its last page, 0 when unknown */
} Link;

typedef struct {
    const uint8_t *data;
    uint8_t *memory; /* for stb_vorbis, after this structure */
    int32_t memory_size;
    stb_vorbis *v;
    int32_t channels, rate;
    int32_t count, current; /* current == count: ended */
    Link links[SOSU_OGG_LINKS];
} Chain;

static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The links of the `size` bytes at `data`, read page by page, every length checked. A page cut or
   damaged ends the scan: what follows belongs to the last link, whose decoder stops where it can.
   A page that begins a stream right after another (streams grouped, as Vorbis beside Theora) does
   not begin a link; the granule positions counted are those of the first stream of the link. */
static void find_links(Chain *c, const uint8_t *data, uint32_t size) {
    uint32_t at = 0, serial = 0, end_of_last = size;
    int audio = 0; /* the current link has a page that does not begin a stream */
    c->count = 1;
    c->links[0].offset = 0;
    c->links[0].frames = 0;
    while (size - at >= 27 && memcmp(data + at, "OggS", 4) == 0) {
        uint32_t segments = data[at + 26], body = 0;
        if (size - at - 27 < segments) break;
        for (uint32_t i = 0; i < segments; i++) body += data[at + 27 + i];
        if (size - at - 27 - segments < body) break;
        if (data[at + 5] & 0x02) {
            if (audio) {
                if (c->count == SOSU_OGG_LINKS) {
                    end_of_last = at; /* more links than kept: the sound ends with the last kept */
                    break;
                }
                c->links[c->count - 1].size = at - c->links[c->count - 1].offset;
                c->links[c->count].offset = at;
                c->links[c->count].frames = 0;
                c->count++;
                audio = 0;
                serial = read32(data + at + 14);
            } else if (at == c->links[c->count - 1].offset) {
                serial = read32(data + at + 14);
            }
        } else {
            uint32_t lo = read32(data + at + 6), hi = read32(data + at + 10);
            audio = 1;
            if (read32(data + at + 14) == serial && !(lo == 0xFFFFFFFFu && hi == 0xFFFFFFFFu))
                c->links[c->count - 1].frames = hi ? 0 : lo; /* stb_vorbis counts in 32 bits */
        }
        at += 27 + segments + body;
    }
    c->links[c->count - 1].size = end_of_last - c->links[c->count - 1].offset;
}

/* Opens the link `k` in place of the current one: 1 when it could, with the channels and the rate
   of the first; else 0, and the sound has ended. */
static int open_link(Chain *c, int32_t k, int *code) {
    stb_vorbis_alloc alloc;
    stb_vorbis_info info;
    if (c->v) stb_vorbis_close(c->v);
    c->v = NULL;
    c->current = c->count;
    if (k < 0 || k >= c->count) return 0;
    alloc.alloc_buffer = (char *)c->memory;
    alloc.alloc_buffer_length_in_bytes = c->memory_size;
    c->v = stb_vorbis_open_memory(c->data + c->links[k].offset, (int)c->links[k].size, code, &alloc);
    if (!c->v) return 0;
    info = stb_vorbis_get_info(c->v);
    if (k > 0 && (info.channels != c->channels || (int32_t)info.sample_rate != c->rate)) {
        stb_vorbis_close(c->v);
        c->v = NULL;
        return 0;
    }
    c->channels = info.channels;
    c->rate = (int32_t)info.sample_rate;
    c->current = k;
    return 1;
}

/* The decoder of the Ogg Vorbis file of `size` bytes at `data`, or NULL with `*error` set:
   1 when `memory` is too small, 2 when the file is damaged or not Ogg Vorbis. */
void *sosu_audio_ogg_open(const uint8_t *data, int32_t size, uint8_t *memory, int32_t memory_size,
                          int32_t *error) {
    int code = 0;
    uintptr_t skip = (sizeof(Chain) + 15) & ~(uintptr_t)15;
    Chain *c;
    skip += (16 - ((uintptr_t)memory + skip) % 16) % 16;
    if (size <= 0 || memory_size < 0 || (uintptr_t)memory_size <= skip) {
        *error = size <= 0 ? 2 : 1;
        return NULL;
    }
    c = (Chain *)memory;
    memset(c, 0, sizeof *c);
    c->data = data;
    c->memory = memory + skip;
    c->memory_size = memory_size - (int32_t)skip;
    find_links(c, data, (uint32_t)size);
    if (!open_link(c, 0, &code)) {
        *error = code == VORBIS_outofmem ? 1 : 2;
        return NULL;
    }
    if (c->count == 1) {
        /* one link: its length as stb_vorbis finds it, once, while its decoder is at its start */
        unsigned int n = stb_vorbis_stream_length_in_samples(c->v);
        c->links[0].frames = n == SAMPLE_unknown ? 0 : n;
    }
    *error = 0;
    return c;
}

int32_t sosu_audio_ogg_channels(void *p) {
    return ((Chain *)p)->channels;
}

int32_t sosu_audio_ogg_rate(void *p) {
    return ((Chain *)p)->rate;
}

/* Decodes up to `floats / channels` frames into `out`, interleaved; gives the frames decoded,
   0 at the end of the last link (or when it is damaged further on). */
int32_t sosu_audio_ogg_read(void *p, int32_t channels, float *out, int32_t floats) {
    Chain *c = (Chain *)p;
    int code = 0;
    while (c->v) {
        int32_t n = stb_vorbis_get_samples_float_interleaved(c->v, channels, out, floats);
        if (n > 0) return n;
        if (!open_link(c, c->current + 1, &code)) return 0;
    }
    return 0;
}

/* Back to the start of the file, for a sound that loops (audio.stream): 1 when it could. */
int32_t sosu_audio_ogg_rewind(void *p) {
    Chain *c = (Chain *)p;
    int code = 0;
    if (c->v && c->current == 0) return stb_vorbis_seek_start(c->v) ? 1 : 0;
    return open_link(c, 0, &code);
}

/* The frames of the file, 0 when unknown: what stb_vorbis finds for a single link, the sum of the
   granule positions of their last pages for several. */
int64_t sosu_audio_ogg_length(void *p) {
    Chain *c = (Chain *)p;
    int64_t total = 0;
    for (int32_t k = 0; k < c->count; k++) {
        if (c->links[k].frames == 0) return 0;
        total += c->links[k].frames;
    }
    return total;
}

/* To the frame `frame` of the file, in the link that holds it: 1 when it could. */
int32_t sosu_audio_ogg_seek(void *p, int64_t frame) {
    Chain *c = (Chain *)p;
    int code = 0;
    int64_t start = 0;
    if (frame < 0) return 0;
    if (c->count == 1) {
        if (frame > 0xFFFFFFFFll || (!c->v && !open_link(c, 0, &code))) return 0;
        return stb_vorbis_seek(c->v, (unsigned int)frame) ? 1 : 0;
    }
    for (int32_t k = 0; k < c->count; k++) {
        int64_t length = c->links[k].frames;
        if (length == 0) return 0; /* a link of unknown length: where the next starts is unknown */
        if (frame < start + length) {
            if ((c->current != k || !c->v) && !open_link(c, k, &code)) return 0;
            return stb_vorbis_seek(c->v, (unsigned int)(frame - start)) ? 1 : 0;
        }
        start += length;
    }
    return 0;
}

void sosu_audio_ogg_close(void *p) {
    Chain *c = (Chain *)p;
    if (c->v) stb_vorbis_close(c->v);
    c->v = NULL;
}
