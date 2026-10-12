# Collyn development fork

The `dev` branch starts at `quinovax/waydroid-nvidia` commit
`a2fb681b0e6c`. `main` remains at the fork's original release history.
These commits port backend fixes developed while integrating AnvilDroid.
AnvilDroid's window decoration, runtime lifecycle and GUI changes belong to
AnvilDroid and are not part of this backend fork.

## Changes

- **RGB32 pitch admission:** require 256-byte row alignment for RGBA8888,
  RGBX8888 and BGRA8888 imports, including framebuffer targets. For these
  formats this means 64 pixels, not 256 pixels: 1920 and 1088 are valid
  pixel strides. Misaligned buffers take the existing composition/readback
  fallback. Cursor handling, modifier checks and other format policies are
  preserved. The predicate lives in `src/hwc_pitch.h`; the build installs
  this canonical header into the patched source tree.
- **Vulkan buffer external memory:** when a guest omits
  `VkExternalMemoryBufferCreateInfo`, declare the host allocator's preferred
  export type (DMA_BUF, otherwise OPAQUE_FD) at buffer creation. This addresses
  `VUID-VkBindBufferMemoryInfo-memory-02726`, observed during investigation of
  `quinovax/waydroid-nvidia#11`. Use one handle type to avoid incompatible
  masks (`VUID-VkBufferCreateInfo-pNext-00920`). Guest-provided external info
  remains intact; the added chain uses a local create-info copy and restores
  the dispatcher argument after creation, including on errors. This does
  not claim to resolve every cause of NVIDIA Xid 69 or external-image binding.
- **Guest C++ compatibility:** compile HWC with the provisioned Android 13
  libc++ headers and disable the NDK's default C++ header search. HWC already
  links to the image's Android 13 libc++; headers must match that ABI rather
  than the newer NDK runtime. `provision.sh` already pins these headers.
  Modern Bionic also provides inline narrow `ctype *_l` definitions that
  conflict with the old libc++ fallbacks. Target-header compile probes check
  all 14 functions for availability and inline-definition compatibility.
  When they conflict, a generated build-local overlay removes only those
  14 fallbacks. Wide-character, collation and time fallbacks remain intact;
  older headers retain the original AOSP behavior. No provisioned source
  headers are modified.

- **Hybrid presentation memory:** carry the existing AnvilDroid hybrid fix
  into this fork: when only a non-NVIDIA GPU has a connected display, remap
  dedicated images to coherent host-visible memory only if the image's actual
  memory requirements permit that type. Device-only images retain the guest
  choice. Image creation declares the allocator's external handle using the
  same local-chain strategy as buffers, avoiding NVIDIA SYSMEM KIND faults.
  Per-allocation diagnostic spam and unused experimental fields are omitted.

- **Window constructor event queue:** full UI can be created after the default
  Wayland dispatch thread is running. Construction now waits for xdg configure
  on a private queue, then restores its proxies to the default queue before
  destroying the queue. The concurrent default dispatcher cannot consume those
  construction configures while the caller waits with `windowsMutex` held.
  During AnvilDroid integration, the CI HWC binary stalled SurfaceFlinger in
  `window::create` → `wl_display_roundtrip_queue`; the old payload and the same
  fork source built with existing static dependencies did not stall. This
  change removes that shared-queue synchronization dependency.

## Validation performed

On 2026-10-12, an Ubuntu 24.04 container with the host build dependencies
from CI passed:

- RGB32 admission tests using the actual canonical header, compiled with
  `-Wall -Wextra -Werror`.
- 16 buffer-dispatch cases compiled from the actual patched function:
  DMA_BUF/OPAQUE_FD capability combinations, existing guest external info,
  and success/error results. These verify the handle type, chain preservation,
  single external-info entry and restored dispatcher argument.
- Full Venus renderer build, including `virgl_test_server` and
  `virgl_render_server`.
- Full HWC compile/link and stripping against the Android guest libraries.
- YAML parsing and clean application of the generated patch series from the
  pinned upstream sources. The clean trees match the compiled source trees.

Initial HWC validation used NDK **r26d**. A follow-up reproduced the CI
failure (14 narrow `ctype *_l` redefinitions) using the CI-pinned **r29**
archive verified against `NDK_ZIP_SHA256`. With the locale overlay fix,
full HWC compile/link/strip passed on both **r29** and **r26d** in Ubuntu
24.04. The builds also reused the same output directory to verify that a
stale r29 overlay does not affect the older-header path.

These builds use existing provisioned Android headers/static dependencies/
image libraries mounted read-only. They do not validate a fresh download/
provisioning of all HWC prerequisites. Mesa, ANGLE and
SurfaceFlinger were unchanged and were not rebuilt for this transfer.

The buffer/image dispatcher suite now covers 32 cases; an additional 64-case
selector test covers all masks against device-only, visible, and coherent
memory types.

The equivalent earlier AnvilDroid payload was exercised on an RTX 2060 in
an AMD/NVIDIA hybrid KDE Wayland session. The newly built fork binaries
have not replaced that installed payload. The OPAQUE_FD-only path is
covered by the dispatcher test, not a hardware smoke test. Before a release
or upstream PR, verify the fork payload on hardware: full UI, app windows,
resize/maximize, repeated open/close and restart, plus rendering and GPU
error logs. Include an OPAQUE_FD-only host if available.

## Reproduce the focused checks

With the pinned virgl source and patches 0001–0006 applied:

```sh
./tests/run-hwc-pitch-test.sh
python3 tests/run-vkr-buffer-test.py /path/to/virgl-src
python3 tests/run-vkr-hybrid-test.py /path/to/virgl-src
./build/virglrenderer/build.sh /path/to/virgl-src /path/to/virgl-build
WNV=/path/to/prereqs ROOTFS=/path/to/prereqs/image-rootfs NDK=/path/to/ndk \
  ./build/hwcomposer/build.sh /path/to/hwc-src/hwcomposer /path/to/hwc-build
./tests/repo-hygiene.sh
```

Use `docs/building.md`, the pins in `packaging/ci/`, and
`build/hwcomposer/provision.sh` to prepare source trees and prerequisites.
Both portable regression tests also run in their respective CI build jobs.
Generated patch files contain unified-diff context lines consisting of a
single space; check source whitespace separately from those patch markers.

## Publishing

Push development commits only to `collyn/waydroid-nvidia:dev`. Do not move
`main` or publish release tags until hardware validation is complete.
The inherited installer still downloads upstream releases; this fork has
no release assets yet. Release URL changes and AnvilDroid payload pinning
must happen with a real, verified fork release, not merely a branch push.
