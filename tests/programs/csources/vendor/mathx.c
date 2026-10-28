/* A C library given with the package as its sources ([link] c, spec §9.1). */
#include "mathx_config.h"

int mathx_scale(int x) {
    return x * MATHX_FACTOR + MATHX_OFFSET; /* MATHX_OFFSET: from `defines` */
}
