# Transport design: extending vtest for Venus-on-NVIDIA

The Venus guest driver talks to the host over Mesa's **vtest** unix-socket
transport (dev-quality, normally used for testing). Stock vtest lacks three
things HWUI/gralloc need. Each was added as a small, symmetric patch pair
(mesa guest side ↔ virglrenderer host side), fd-passing done with `SCM_RIGHTS`.

## 1. sync_fd over vtest (`VK_KHR_external_{fence,semaphore}_fd`)

**Why:** guest Venus gates the extensions on `renderer->info.has_external_sync`;
vtest set it false. HWUI hard-requires `VK_KHR_external_semaphore_fd`
(`VulkanManager` asserts on it) to sync with the compositor.

**Host (virglrenderer):**
- `RENDER_CONTEXT_OP_EXPORT_FENCE {fence_id}` → reply `{found}` + one fd.
- vkr dispatch: lock queues, find the pending sync by `fence_id`, dup the
  SYNC_FD cached at submit (one `vkGetFenceFdKHR` before the waiter thread
  sees the fence). Not found ⇒ already retired ⇒ `found=0` (guest returns
  fd −1 = "already signaled", spec-legal). A second GetFenceFdKHR races the
  waiter; NVIDIA then returns `VK_ERROR_INVALID_EXTERNAL_HANDLE` and the
  VkFence never signals.
- Public API `virgl_renderer_context_export_fence(ctx, fence_id, &fd)` (bypasses
  the DRM/GL-only fence table).
- vtest: `VCMD_SYNC_EXPORT_SYNC_FILE` + `VCMD_PARAM_HAS_VENUS_SYNC_FD`.

**Guest (mesa `vn_renderer_vtest.c`):** query the param → `has_external_sync =
true`; implement `export_syncobj` (fd, or −1 on SIGNALED). `vn_queue.c` needs no
changes (all transport-side). `vn_create_sync_file` accepts fd == −1.

**Gotcha:** exporting a fence SYNC_FD **resets the fence** (copy-transference) —
never wait on the fence after export.

Verified host-side (`tests/`): sync_fd through the socket signals in 0.67 ms,
identical to direct NVIDIA.

## 2. dma_buf import over vtest (`VK_ANDROID_external_memory_android_hardware_buffer`)

**Why:** HWUI's `SkSurface::MakeFromAHardwareBuffer` →
`vn_get_memory_dma_buf_properties` hit a NULL renderer op; vtest had no import path.

- `VCMD_RESOURCE_IMPORT_BLOB`: client sends the dmabuf fd via `SCM_RIGHTS` +
  res_id; server imports and attaches (size-0 → `lseek`). Server plumbing reused
  `RENDER_CONTEXT_OP_IMPORT_RESOURCE` / `virgl_renderer_resource_import_blob`.
- mesa: `vtest_bo_create_from_dma_buf` + `has_dma_buf_import = true` + the
  dma_buf properties query path.

**Two real bugs fixed here:** (1) `virgl_resource_create_from_fd` **takes
ownership** of the fd — closing it forwarded a dead fd (intermittent
`invalid res_id` → CS-fatal context → vkCreateDevice crash-loop). (2) import must
be visible before the client gets the res_id — the ring-0 export_fence became a
wire round-trip barrier.

## 3. host-side GPU allocation (`VCMD_RESOURCE_ALLOC_GPU`)

**Why:** the display finding — guest buffers must be **NVIDIA-native** or KWin
can't bind them. So gralloc allocates *through the host*.

- New host allocator `src/virglrenderer-vtest/vtest_gpu_alloc.c`: GPU buffers as
  exportable `VkImage`s. On a NVIDIA compositor they use real NVIDIA
  **block-linear** DRM modifiers (LINEAR is external-only on NVIDIA EGL ⇒
  undisplayable; block-linear binds as `GL_TEXTURE_2D`). On an iGPU compositor
  (hybrid) they use **NVIDIA LINEAR**, which Intel/AMD can import. CPU-mappable
  buffers as NVIDIA linear images (or udmabuf fallback).
- Guest gralloc backend `src/minigbm-vtest/vtest_wrapper.c` replaces
  `libgbm_mesa_wrapper.so` — allocations go over the socket instead of guest libgbm.

## AHB / ANB memory-type fixes (mesa `0003-wip`)

ANGLE adds `VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT` to every sampled AHB image
*and* every Android swapchain image (`ColorNeedsInputAttachmentUsage`).
NVIDIA rejects LINEAR-modifier CreateImage / dedicated import with that
usage (`VK_ERROR_FORMAT_NOT_SUPPORTED` = −11). On a hybrid laptop the
present path *is* LINEAR, so HWUI window surfaces (SystemUI, Settings)
abort with `vkCreateImage w/ native buffer failed` then
`drawRenderNode called on a context with no surface` (issue #7).
Guest-mesa fixes:

- Force `DRM_FORMAT_MODIFIER` tiling in deferred AHB init (older ANGLE passes
  legacy LINEAR).
- Strip `INPUT_ATTACHMENT` for LINEAR-modifier AHB *and* `VK_ANDROID_native_buffer`
  (ANB / swapchain) wraps. Nothing actually input-attaches those images.
- Accept LINEAR tiling in AHB format queries.
- UMA-style `DEVICE_LOCAL` on all memory types/heaps.

## Global-priority strip/retry (virglrenderer `vkr_device.c`)

HWUI requests a global queue priority
(`VkDeviceQueueGlobalPriorityCreateInfoKHR`); NVIDIA returns
`VK_ERROR_NOT_PERMITTED_KHR` for unprivileged REALTIME/HIGH, but Android expects
best-effort. On `NOT_PERMITTED`, strip the priority structs from the queue pNext
chains and retry `vkCreateDevice` once.

## Performance characteristics

- The socket transport adds per-op overhead vs direct calls; Vulkan batches by
  design, so few submits per frame keep this acceptable.
- Plain host-visible allocations are steered to a CACHED memory type of the same
  heap (write-combined sysmem through the socket is slow otherwise).
- dma_buf mmap read bandwidth is below native — relevant to readback / lockCanvas
  paths, not the hot render path.

## Hybrid presentation copy (`VCMD_PRESENT_CREATE` / `VCMD_PRESENT_COPY`)

On hosts where rendering into NVIDIA LINEAR buffers causes device loss, keep
`WAYDROID_NVIDIA_PRESENT=block-linear`. The HWC now handles an AMD/Intel
compositor that cannot import those tiled buffers without reading the whole
frame back through EGL. Producer rendering remains tiled; only the final
presentation copy uses LINEAR. Direct compositor imports retain their usual
path. Unsupported formats, old hosts, or failed copy setup retain wl_shm.

- Command 46 (`CREATE`) uses a dedicated, uninitialized vtest context. Its
  seven DWORDs are width, height, DRM format, source stride, source offset,
  and modifier low/high, followed by a source fd via SCM_RIGHTS. The reply
  contains status and destination stride, followed by three destination fds
  on success. Only single-plane 32-bit RGB formats and bounded sizes are
  supported. Disconnect releases all imported images and copy resources.
- Command 47 (`COPY`) takes a destination index (0..2), returning status after
  Vulkan fence completion. The host imports the tiled source once and copies
  it with `vkCmdCopyImage` into an exportable, pure-system-memory NVIDIA
  LINEAR image. Destination allocation width is padded to 64 pixels (256-byte rows), while
  source/copy/Wayland extents stay unchanged. This prevents freeform/shadow
  buffers with 32-pixel pitches from silently falling back to CPU readback.
  No rendering or color attachment targets LINEAR. External
  queue ownership and layout transitions bracket each transfer.
- HWC waits the Android producer acquire fence first, caches the source and
  three destinations, and admits only slots released by the compositor.
  `wl_buffer.release` uses a private event queue dispatched exclusively on
  the HWC thread. If all slots are busy, that presentation is skipped rather
  than overwriting compositor data. Failed copies revert the cached source
  to wl_shm. NVIDIA compositor modifiers exclude this cross-GPU path because
  NVIDIA EGL cannot sample its LINEAR buffers as sampler2D.
- Socket I/O is bounded, suppresses SIGPIPE, receives CLOEXEC fds, and rejects
  and closes unexpected/truncated ancillary data. No commands in an existing
  guest Mesa/gralloc connection change.

Regression checks:

```sh
./tests/run-present-wire-test.sh  # portable fd/ring/disconnect checks; also CI
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation ./tests/run-present-copy-test.sh
# Optional PRESENT_TEST_SOCKET=/path/to/isolated/vtest.sock tests commands 46/47.
```

The GPU test requires NVIDIA plus an AMD/Intel render node. It checks 24
changing frames across all three slots at 256x256, 1984x1164 and 1088x1920,
plus odd/freeform widths 600, 664 and 563, including actual shader sampling
on the other GPU. Every destination pitch must be 256-byte aligned. On RTX 2060 + RX 6600,
driver 615.71.09, the 1984x1164 transfer took about 0.8 ms. A controlled
launcher scroll sample improved from 43.7% janky frames / 72.6 ms median to
0% / 1.8 ms. These are sample measurements, not guarantees for every app.
