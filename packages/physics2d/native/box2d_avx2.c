/* Box2D's contact solver of 8 lanes (AVX2), which its build compiles alone with /arch:AVX2 under
   MSVC; it is left out of [native.box2d] and compiled here. Box2D calls it only when the processor
   has AVX2 (simd.c reads CPUID); MSVC emits the AVX2 intrinsics without /arch:AVX2, so the guard
   of the file is satisfied rather than the whole program built for AVX2. gcc and clang turn AVX2
   on for its functions alone (Box2D's B2_AVX2_BEGIN). */
#if defined(_MSC_VER) && !defined(__clang__) && !defined(__AVX2__) && (defined(_M_X64) || defined(_M_IX86))
#define __AVX2__ 1
#endif
#include "contact_solver_w8.c"
