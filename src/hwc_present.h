/* SPDX-License-Identifier: MIT */
#pragma once
#include "wayland-hwc.h"
struct hwc_present;
std::unique_ptr<buffer> create_present_wl_buffer(display *, const buffer_metadata &, buffer_handle_t);
int update_present_buffer(buffer *); // 0 copied, 1 all slots in use, -1 failed
void present_buffer_attached(buffer *);
void destroy_present_buffer(hwc_present *);
