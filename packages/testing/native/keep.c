/* testing.bench.keep (spec §8.3): a value given to a function of another file, which the C
   compiler cannot see into, so that the work of a benchmark that computes it is not removed. */
#include <stdint.h>

volatile double sosu_testing_kept;

void sosu_testing_keep(double value) {
    sosu_testing_kept = value;
}

/* testing.fixture: stops the test with a message. A Sosu `extern` of sosu_fail would declare it
   with `uint8_t *` parameters, which conflicts with its declaration in sosu_rt.h for gcc and
   clang. */
void sosu_fail(const char *loc, const char *message);

void sosu_testing_fail(uint8_t *place, uint8_t *message) {
    sosu_fail((const char *)place, (const char *)message);
}
