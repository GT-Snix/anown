// Headless test: hex color conversion.
#include "anown.h"

#include <math.h>
#include <stdio.h>

static int failures = 0;

#define CHECK(cond)                                                   \
    do {                                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,    \
                    __LINE__, #cond);                                 \
            failures++;                                               \
        }                                                             \
    } while (0)

static int near(float a, float b) { return fabsf(a - b) < 1e-6f; }

int main(void) {
    an_color c = an_hex(0xF3EDE2);
    CHECK(near(c.r, 0xF3 / 255.0f));
    CHECK(near(c.g, 0xED / 255.0f));
    CHECK(near(c.b, 0xE2 / 255.0f));
    CHECK(near(c.a, 1.0f));

    an_color k = AN_CREAM;
    CHECK(near(k.r, c.r) && near(k.g, c.g) && near(k.b, c.b));

    if (failures) return 1;
    printf("test_color: ok\n");
    return 0;
}
