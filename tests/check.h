// Minimal test helpers. Each test is its own program; return check_done(name).
#ifndef ANOWN_TEST_CHECK_H
#define ANOWN_TEST_CHECK_H

#include <math.h>
#include <stdio.h>

static int check_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            check_failures++;                                                  \
        }                                                                      \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                  \
    do {                                                                       \
        const double check_a_ = (a), check_b_ = (b);                           \
        if (fabs(check_a_ - check_b_) > (eps)) {                               \
            fprintf(stderr, "%s:%d: CHECK_NEAR failed: %s = %g, %s = %g\n",    \
                    __FILE__, __LINE__, #a, check_a_, #b, check_b_);           \
            check_failures++;                                                  \
        }                                                                      \
    } while (0)

static inline int check_done(const char *name) {
    if (check_failures) {
        fprintf(stderr, "%s: %d check(s) failed\n", name, check_failures);
        return 1;
    }
    printf("%s: ok\n", name);
    return 0;
}

#endif
