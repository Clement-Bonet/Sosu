/* Sozo's side of the contract (engine.host, spec 9.2): a C host that drives the game `crates` as
   the editor does: its kinds read, entities made and changed, the scene saved and loaded, Play and
   Stop, the logs, and a failing step of the game read as an error while the host goes on. */
#include <stdio.h>
#include <string.h>

#include "crates.h"

static sosu_str s(const char *text) {
    sosu_str r = { (const uint8_t *)text, strlen(text) };
    return r;
}

static int same(sosu_str a, const char *b) { return a.len == strlen(b) && memcmp(a.ptr, b, a.len) == 0; }

static int contains(sosu_str a, const char *b) {
    size_t n = strlen(b);
    for (size_t i = 0; i + n <= a.len; i++) {
        if (memcmp(a.ptr + i, b, n) == 0) return 1;
    }
    return 0;
}

static uint64_t kind_named(engine_Game *g, const char *name) {
    uint64_t count = sozo_kind_count(g);
    for (uint64_t i = 0; i < count; i++) {
        if (same(sozo_kind_name(g, i), name)) return i;
    }
    return count;
}

static uint64_t field_named(engine_Game *g, uint64_t kind, const char *name) {
    uint64_t count = sozo_field_count(g, kind);
    for (uint64_t i = 0; i < count; i++) {
        if (same(sozo_field(g, kind, i).name, name)) return i;
    }
    return count;
}

static engine_Value number(float x) {
    engine_Value v = { 0 };
    v.tag.tag = engine_Tag_Number;
    v.number = x;
    return v;
}

static engine_Value flag(bool on) {
    engine_Value v = { 0 };
    v.tag.tag = engine_Tag_Flag;
    v.flag = on;
    return v;
}

int main(void) {
    printf("contract %u\n", sozo_contract());
    engine_Game *g = sozo_open();
    printf("kinds before setup %llu\n", (unsigned long long)sozo_kind_count(g));
    sozo_setup(g);
    uint64_t kinds = sozo_kind_count(g);
    printf("kinds %llu, the last %.*s\n", (unsigned long long)kinds, (int)sozo_kind_name(g, kinds - 1).len,
           (const char *)sozo_kind_name(g, kinds - 1).ptr);

    /* a crate with a body, its fields described */
    scene_Entity crate = sozo_spawn(g, s("crate"));
    math_Transform t = sozo_transform(g, crate);
    t.position.y = 5.0f;
    sozo_set_transform(g, crate, t);
    uint64_t body = kind_named(g, "body");
    sozo_add(g, crate, body);
    uint64_t shape = field_named(g, body, "shape");
    engine_FieldInfo info = sozo_field(g, body, shape);
    sosu_str sphere = sozo_choice(g, body, shape, 1);
    printf("body.shape: %.*s, kind %u, %llu choices, the second %.*s\n", (int)info.label.len, (const char *)info.label.ptr,
           info.kind.tag, (unsigned long long)info.choices, (int)sphere.len, (const char *)sphere.ptr);
    sozo_set(g, crate, body, field_named(g, body, "mass"), number(4.0f));
    printf("mass %.1f\n", sozo_get(g, crate, body, field_named(g, body, "mass")).number);

    /* a lift of the game under it, kept where it is in the world */
    scene_Entity lift = sozo_spawn(g, s("lift"));
    printf("parented %d\n", sozo_set_parent(g, lift, crate, true));
    printf("lift local y %.1f, world y %.1f\n", sozo_transform(g, lift).position.y, sozo_world_matrix(g, lift).m[13]);
    uint64_t lifts = kind_named(g, "lift");
    sozo_add(g, lift, lifts);
    sozo_set(g, lift, lifts, 0, number(2.0f));

    /* saved, cleared, loaded back */
    sosu_str saved = sozo_save(g);
    static char copy[16384];
    if (saved.len >= sizeof copy) return 1;
    memcpy(copy, saved.ptr, saved.len);
    sosu_str kept = { (const uint8_t *)copy, saved.len };
    sozo_clear(g);
    printf("cleared %llu\n", (unsigned long long)sozo_count(g));
    bool loaded = sozo_load(g, kept); /* before the count: the order of the arguments of printf is not fixed */
    printf("loaded %d, %llu entities\n", loaded, (unsigned long long)sozo_count(g));
    sosu_slice_scene_Entity all = sozo_entities(g);
    crate = all.ptr[0];
    lift = all.ptr[1];
    printf("lift speed %.1f\n", sozo_get(g, lift, kind_named(g, "lift"), 0).number);

    /* Play: the crate falls, the game lifts and says so; Stop: back as saved */
    sozo_play(g);
    for (int i = 0; i < 20; i++) sozo_step(g, 1.0f / 20.0f);
    float fallen = sozo_transform(g, crate).position.y;
    sosu_str logs = sozo_logs(g);
    const char *error = crates_error();
    printf("playing %d, fell %d, logs %d, error %s\n", sozo_playing(g), fallen < 5.0f, contains(logs, "lifted"),
           error ? error : "none");
    sozo_stop(g);
    crate = sozo_entities(g).ptr[0];
    printf("stopped: playing %d, y %.1f\n", sozo_playing(g), sozo_transform(g, crate).position.y);

    /* a step of the game that fails a check: an error, and the host goes on */
    lift = sozo_entities(g).ptr[1];
    bool broke = sozo_set(g, lift, kind_named(g, "lift"), 1, flag(true));
    sozo_play(g);
    sozo_step(g, 0.05f);
    error = crates_error();
    printf("broken step: set %d, %s\n", broke,
           error && strstr(error, "out of bounds") ? "index out of bounds" : error ? error : "none");
    sozo_stop(g);
    error = crates_error();
    printf("after it: %llu entities, error %s\n", (unsigned long long)sozo_count(g), error ? error : "none");

    /* a scene that does not read */
    bool bad = sozo_load(g, s("{ nope"));
    printf("bad scene %d, %llu entities, said %d\n", bad, (unsigned long long)sozo_count(g), sozo_message(g).len > 0);
    sozo_close(g);
    return 0;
}
