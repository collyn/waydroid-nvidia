/* SPDX-License-Identifier: MIT */
#pragma once
static inline unsigned hwc_present_free_slot(unsigned busy, unsigned next) {
    if(next>=3)return 3;
    for(unsigned i=0;i<3;i++) {
        unsigned slot=(next+i)%3;
        if(!(busy&(1u<<slot)))return slot;
    }
    return 3;
}
