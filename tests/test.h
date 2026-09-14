#ifndef BYPO_TEST_H
#define BYPO_TEST_H

#include <math.h>
#include <stdio.h>

extern int test_failures;
extern int test_checks;

#define CHECK(cond, ...)                                            \
    do {                                                            \
        test_checks++;                                              \
        if (!(cond)) {                                              \
            test_failures++;                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);             \
            printf(__VA_ARGS__);                                    \
            printf("\n");                                           \
        }                                                           \
    } while (0)

#define CHECK_NEAR(a, b, tol, ...) CHECK(fabsf((a) - (b)) < (tol), __VA_ARGS__)

#endif
