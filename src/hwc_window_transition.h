/* Bounded resize handoffs. A task disappearing outside an explicit resize is
 * still an ordinary window close. All state is protected by windowsMutex. */
#pragma once
#include <stdint.h>
#include <chrono>

inline int64_t hwc_transition_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct hwc_window_transition {
    static constexpr int64_t timeout_ms = 8000;
    static constexpr int64_t maximum_reload_ms = 60000;
    int64_t deadline_ms = 0;
    int64_t reload_deadline_ms = 0;
    bool missing = false;
    bool closing = false;

    bool begin(int64_t now) {
        if (closing) return false;
        deadline_ms = now + timeout_ms;
        reload_deadline_ms = now + maximum_reload_ms;
        return true;
    }
    bool active(int64_t now) const {
        return !closing && deadline_ms > now;
    }
    bool holding(int64_t now) const { return missing && active(now); }
    bool refresh(int64_t now) {
        if (!holding(now)) return false;
        auto next=now+timeout_ms;
        deadline_ms=next<reload_deadline_ms?next:reload_deadline_ms;
        return true;
    }
    void close() { closing = true; deadline_ms = 0; }
    bool handoff(int64_t now, bool old_visible, unsigned package_windows,
                 unsigned package_tasks) const {
        return active(now) && !old_visible && package_windows == 1 && package_tasks == 1;
    }
};
