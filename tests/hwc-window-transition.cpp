#include "../src/hwc_window_transition.h"
#include <cassert>
int main() {
    hwc_window_transition s;
    assert(!s.active(100));
    s.missing=true;
    assert(!s.holding(100)); // Ordinary absence still closes.
    assert(s.begin(100));
    assert(s.holding(100));
    assert(s.handoff(101,false,1,1)); // Unique replacement task.
    assert(!s.handoff(101,true,1,1)); // Original task remains visible.
    assert(!s.handoff(101,false,2,1)); // Two windows of the same package.
    assert(!s.handoff(101,false,1,2)); // Ambiguous new tasks.
    assert(!s.handoff(101,false,1,0)); // No replacement.
    assert(s.holding(100+hwc_window_transition::timeout_ms-1));
    assert(!s.holding(100+hwc_window_transition::timeout_ms));
    assert(s.begin(9000)); // Further resize renews the deadline.
    assert(s.refresh(16000)&&s.holding(20000)); // Slow but live Android reload.
    for (int64_t now=21000;now<69000;now+=1000) assert(s.refresh(now));
    assert(!s.holding(69000)); // A stale task snapshot cannot retain forever.
    assert(s.begin(9000));
    s.missing=false;
    assert(!s.holding(9001)&&s.active(9001)); // Visible content never blanks.
    s.close();
    assert(!s.active(9001));
    assert(!s.begin(9002)); // Pending resize cannot undo explicit Close.
    assert(!s.handoff(9003,false,1,1));
}
