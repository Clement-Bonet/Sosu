/* testing.bench.keep (spec §8.3): a value given to a function of another file, which the C
   compiler cannot see into, so that the work of a benchmark that computes it is not removed. */
volatile double sosu_testing_kept;

void sosu_testing_keep(double value) {
    sosu_testing_kept = value;
}
