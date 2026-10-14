# Resize task handoffs

Multi-window HWC keeps a mapped host window across an explicitly announced
Android resize. A disappearing task's subsurfaces keep their last imported
buffers; a unique replacement task of the same package takes over the existing
window, including its xdg_toplevel, chrome, compositor position and focus.
Rekeying the task does not change the open-window count or trigger calibration.

The optional `anvil_hwc_window_transition_v1(operation, task, package)` C ABI is
for an in-process desktop bridge. Call it outside the bridge's Wayland mutex:
HWC acquires `windowsMutex` and window retirement sends Wayland requests.

- `1`: arm a resize for the exact existing task/package before submitting it.
- `2`: cancel retention on explicit Close. It also covers a unique pending
  package whose Android task changed before its first layer arrived.
- `3`: retire expired, missing windows even if SurfaceFlinger stopped drawing.
- `4`: disarm when the resize request could not be submitted.
- `5`: renew a missing window when Android still reports a visible task for
  the unique package. The maximum reload duration is 60 seconds; stale task
  snapshots cannot leave permanent ghost windows.

The initial grace interval is eight seconds. Ordinary task removal without an
announced resize still closes immediately. Two simultaneous tasks or host
windows of the same package are never merged. Mode switches keep their normal
cleanup semantics. Retained frames remain Wayland buffers; no screenshot or
CPU frame readback is introduced.

Run `bash tests/run-hwc-window-transition-test.sh` for close, ambiguity and
deadline coverage. Live acceptance should record the compositor window ID
before and after a resize that changes Android task ID. Android application
crashes are separate: preserving a host window does not restart a dead app.
