/* SPDX-License-Identifier: Apache-2.0 */
#include "../src/hwc_pitch.h"
#include <assert.h>
int main(void) {
    const unsigned valid[] = {0, 64, 576, 1024, 1088, 1920, 1984};
    const unsigned invalid[] = {1, 25, 65, 600, 1048, 1080};
    for (unsigned i=0; i<sizeof(valid)/sizeof(valid[0]); i++)
        assert(hwc_rgb32_pitch_aligned(valid[i], true));
    for (unsigned i=0; i<sizeof(invalid)/sizeof(invalid[0]); i++) {
        assert(!hwc_rgb32_pitch_aligned(invalid[i], true));
        assert(hwc_rgb32_pitch_aligned(invalid[i], false));
    }
    return 0;
}
