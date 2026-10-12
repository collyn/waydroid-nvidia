#!/usr/bin/env python3
"""Compile the actual patched CreateBuffer dispatcher with a recording Vulkan stub."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def check(tree):
    source = (tree / 'src/venus/vkr_buffer.c').read_text()
    start = source.index('static void\nvkr_dispatch_vkCreateBuffer(')
    body = source.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    dispatcher = source[start:end]
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <vulkan/vulkan.h>
struct vkr_physical_device { bool is_dma_buf_fd_export_supported, is_opaque_fd_export_supported; };
struct vkr_device { struct vkr_physical_device *physical_device; };
struct vn_dispatch_context { void *data; };
struct vn_command_vkCreateBuffer { VkDevice device; const VkBufferCreateInfo *pCreateInfo; VkResult ret; };
static unsigned calls;
static VkExternalMemoryHandleTypeFlags expected;
static const void *expected_tail;
static VkResult create_result;
static struct vkr_device *vkr_device_from_handle(VkDevice device) { return (void *)device; }
static const void *vkr_find_struct(const void *next, VkStructureType type) {
    for (const VkBaseInStructure *p=next; p; p=p->pNext)
        if (p->sType==type) return p;
    return NULL;
}
static void vkr_buffer_create_and_add(void *ctx, struct vn_command_vkCreateBuffer *args) {
    (void)ctx;
    calls++;
    const VkExternalMemoryBufferCreateInfo *info=vkr_find_struct(args->pCreateInfo->pNext,
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO);
    assert((info ? info->handleTypes : 0)==expected);
    unsigned count=0;
    for (const VkBaseInStructure *p=args->pCreateInfo->pNext; p; p=p->pNext)
        count += p->sType==VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
    assert(count==(expected ? 1u : 0u));
    if (info) assert(info->pNext==expected_tail);
    else assert(args->pCreateInfo->pNext==expected_tail);
    args->ret=create_result;
}
'''
    tests = r'''
int main(void) {
    VkBufferOpaqueCaptureAddressCreateInfo tail={.sType=VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO};
    for (unsigned dma=0; dma<2; dma++) for (unsigned opaque=0; opaque<2; opaque++) {
        struct vkr_physical_device physical={dma,opaque};
        struct vkr_device device={&physical};
        struct vn_dispatch_context ctx={0};
        for (unsigned guest=0; guest<2; guest++) for (unsigned fail=0; fail<2; fail++) {
            const VkExternalMemoryBufferCreateInfo guest_info={
                .sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                .pNext=&tail,.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
            const VkBufferCreateInfo info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .pNext=guest ? (const void *)&guest_info : (const void *)&tail};
            struct vn_command_vkCreateBuffer args={.device=(VkDevice)&device,.pCreateInfo=&info};
            calls=0;expected_tail=&tail;
            expected=guest ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT : dma
                ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT : opaque
                ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT : 0;
            create_result=fail ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_SUCCESS;
            vkr_dispatch_vkCreateBuffer(&ctx,&args);
            assert(calls==1 && args.ret==create_result);
            assert(args.pCreateInfo==&info);
            assert(info.pNext==(guest ? (const void *)&guest_info : (const void *)&tail));
            assert(guest_info.pNext==&tail && tail.pNext==NULL);
        }
    }
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='vkr-buffer-test-') as directory:
        test = Path(directory) / 'test.c'
        binary = Path(directory) / 'test'
        test.write_text(harness + dispatcher + tests)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                        str(test), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print('PASS: 16 CreateBuffer export/chain/error cases')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tree', type=Path, help='Virglrenderer tree with the repository patch series applied')
    check(parser.parse_args().tree)
