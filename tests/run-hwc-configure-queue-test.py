#!/usr/bin/env python3
"""Compile the real HWC constructor queue scope and verify proxy lifetime ordering."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('tree',type=Path)
args=parser.parse_args()
source=(args.tree/'hwcomposer/wayland-hwc.cpp').read_text()
start=source.index('    struct configure_queue {')
end=source.index('    } init_queue ',start)+5
scope=source[start:end]+';'
code=r'''
#include <cassert>
#include <cstddef>
struct wl_event_queue { bool live=true; };
struct wl_proxy { wl_event_queue *queue=nullptr; bool live=true; };
static unsigned reset, destroyed;
static void wl_proxy_set_queue(wl_proxy *p, wl_event_queue *q) {
    assert(p->live && (!q || q->live)); p->queue=q; if(!q) ++reset;
}
static void wl_event_queue_destroy(wl_event_queue *q) { assert(q->live); q->live=false; ++destroyed; }
int main() {
'''+scope+r'''
    wl_event_queue queue;
    wl_proxy surface,viewport,xdg,top,scale;
    {
        configure_queue scope{&queue};
        scope.add(&surface);scope.add(&viewport);scope.add(&xdg);scope.add(&top);scope.add(&scale);
        assert(surface.queue==&queue && top.queue==&queue);
        scope.remove(&scale);scale.live=false;
    }
    assert(reset==4 && destroyed==1 && !queue.live);
    assert(!surface.queue && !viewport.queue && !xdg.queue && !top.queue);
    { configure_queue empty{nullptr}; empty.add(nullptr); }
    assert(destroyed==1);
}
'''
with tempfile.TemporaryDirectory(prefix='hwc-queue-test-') as directory:
    root=Path(directory);(root/'test.cpp').write_text(code)
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-Wall','-Wextra','-Werror',str(root/'test.cpp'),'-o',str(root/'test')],check=True)
    subprocess.run([str(root/'test')],check=True)
print('PASS: configure queue proxy restoration and destruction ordering')
