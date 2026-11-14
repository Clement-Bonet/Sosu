/* A C program using the Sosu library `geom` (spec §9.2), through its generated header. */
#include <stdio.h>
#include <string.h>

#include "geom.h"

static int32_t twice(int32_t x) { return 2 * x; }

int main(void) {
    geom_Vec2 a = { 1.0f, 2.0f };
    geom_Vec2 b = { 3.0f, 4.0f };
    geom_Vec2 c = geom_scale(a, 3.0f);
    printf("dot %.1f, scale %.1f %.1f\n", geom_dot(a, b), c.x, c.y);

    const char *text = "h\xc3\xa9llo";
    sosu_str s = { (const uint8_t *)text, strlen(text) };
    printf("bytes %llu\n", (unsigned long long)geom_bytes(s));

    int64_t numbers[] = { 2, 3, 4 };
    sosu_slice_int64_t values = { numbers, 3 };
    geom_Mode add = { geom_Mode_Add };
    geom_Mode multiply = { geom_Mode_Multiply };
    printf("combine %lld %lld\n", (long long)geom_combine(values, add), (long long)geom_combine(values, multiply));
    printf("apply %d\n", geom_apply(twice, 5));

    /* a run-time error returns to the host: a zero value, and the message (read after the call) */
    long long found = (long long)geom_at(values, 1);
    printf("at %lld, error: %s\n", found, geom_error() ? geom_error() : "none");
    found = (long long)geom_at(values, 7);
    printf("at %lld, error: %s\n", found, geom_error() && strstr(geom_error(), "out of bounds") ? "index out of bounds" : "none");
    int half = geom_half(7);
    printf("half %d, error: %s\n", half, geom_error() && strstr(geom_error(), "requires failed") ? "requires" : "none");
    half = geom_half(8);
    printf("half %d, error: %s\n", half, geom_error() ? geom_error() : "none");

    /* faults of the processor under the guard: errors too, and the library still works after them */
    long long deep = (long long)geom_deep(0);
    printf("deep %lld, error: %s\n", deep, geom_error() && strstr(geom_error(), "stack overflow") ? "stack overflow" : "none");
    deep = (long long)geom_deep(0); /* again: the stack is whole again after the first */
    printf("deep %lld, error: %s\n", deep, geom_error() && strstr(geom_error(), "stack overflow") ? "stack overflow" : "none");
    int freed = geom_freed();
    printf("freed %d, error: %s\n", freed,
           geom_error() && strstr(geom_error(), "after the free()") ? "used after free" : "none");
    int poked = geom_poke(16);
    printf("poke %d, error: %s\n", poked,
           geom_error() && strstr(geom_error(), "invalid memory access") ? "invalid memory access" : "none");
    float dot = geom_dot(a, b); /* before geom_error(): the order of the arguments of printf is not fixed */
    printf("dot after them %.1f, error: %s\n", dot, geom_error() ? geom_error() : "none");

    geom_Counter *counter = geom_counter_new();
    geom_counter_add(counter, 40);
    geom_counter_add(counter, 2);
    printf("counter %lld\n", (long long)geom_counter_total(counter));
    geom_counter_free(counter);
    return 0;
}
