#!/usr/bin/env python3
"""Compile the actual hybrid memory-type selector against Vulkan headers."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def check(tree):
    source = (tree / 'src/venus/vkr_device_memory.c').read_text()
    start = source.index('static uint32_t\nvkr_hybrid_memory_type(')
    end = source.index('\n}\n', start) + 3
    function = source[start:end]
    harness = r'''
#include <assert.h>
#include <vulkan/vulkan.h>
'''
    tests = r'''
int main(void) {
    VkPhysicalDeviceMemoryProperties p = {.memoryTypeCount=4};
    p.memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    p.memoryTypes[1].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    p.memoryTypes[2].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    p.memoryTypes[3].propertyFlags=p.memoryTypes[2].propertyFlags | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    for (unsigned mask=0; mask<16; mask++) {
        unsigned expected=(mask & 4) ? 2 : (mask & 8) ? 3 : 0;
        assert(vkr_hybrid_memory_type(&p,mask,0)==expected);
        for (unsigned visible=1; visible<4; visible++)
            assert(vkr_hybrid_memory_type(&p,mask,visible)==visible);
    }
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='vkr-hybrid-test-') as directory:
        src = Path(directory) / 'test.c'
        binary = Path(directory) / 'test'
        src.write_text(harness + function + tests)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                        str(src), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print('PASS: 64 hybrid memory-type compatibility cases')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tree', type=Path)
    check(parser.parse_args().tree)
