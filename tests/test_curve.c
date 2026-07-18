#include "../src/curve.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static int near(float left, float right) {
    return fabsf(left - right) < 0.001f;
}

int main(void) {
    assert(ninefan_curve_count == 4);
    for (size_t index = 0; index < ninefan_curve_count; index++) {
        assert(ninefan_curve_is_valid(&ninefan_curves[index]));
    }

    const ninefan_curve *balanced = ninefan_curve_named("balanced");
    assert(balanced != NULL);
    assert(ninefan_curve_named("BALANCED") == balanced);
    assert(ninefan_curve_named("missing") == NULL);

    assert(near(ninefan_curve_fraction(balanced, 20.0f), 0.0f));
    assert(near(ninefan_curve_fraction(balanced, 55.0f), 0.0f));
    assert(near(ninefan_curve_fraction(balanced, 67.0f), 0.35f));
    assert(near(ninefan_curve_fraction(balanced, 72.5f), 0.525f));
    assert(near(ninefan_curve_fraction(balanced, 100.0f), 1.0f));
    assert(near(ninefan_curve_fraction(NULL, 60.0f), 1.0f));

    puts("curve tests passed");
    return 0;
}
