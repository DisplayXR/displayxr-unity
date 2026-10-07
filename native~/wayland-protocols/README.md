# Vendored Wayland protocol XML

- `viewporter.xml` (stable/viewporter)
- `fractional-scale-v1.xml` (staging/fractional-scale)

Copied verbatim from [wayland-protocols](https://gitlab.freedesktop.org/wayland/wayland-protocols)
by way of displayxr-common (`common/linux/wayland-protocols/`). MIT licensed; keep the
upstream copyright block at the top of each file.

**Why vendored:** `libwayland-dev` ships `wayland-scanner` but not the protocol XML, which is
the separate `wayland-protocols` package and is not installed on every dev box or CI image.
Vendoring also pins the protocol revision.

**Used by:** `displayxr_linux_wayland.c`, the native-Wayland player path. The weave is a
sub-surface of the player's window: `wp_viewporter` maps the runtime's device-pixel buffer
onto the logical window size, and `wp_fractional_scale_v1` gives the real scale.

**Regenerating:** don't edit the XML or the generated files. CMake runs `wayland-scanner`
on these files at build time (see `CMakeLists.txt`) into `build-*/wayland-generated/`.
