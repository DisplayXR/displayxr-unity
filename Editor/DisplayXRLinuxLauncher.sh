#!/bin/sh
# @PRODUCT@ launcher, written next to the Linux player by the DisplayXR Unity
# plugin at build time (DisplayXRProviderRuntimeDeploy). Point the app's .desktop
# Exec= line here instead of at the player itself.
#
# It starts the player as a native Wayland app (-force-wayland) where that works,
# which is what gives correct 3D at fractional display scaling, and under X11
# (XWayland) otherwise. Native Wayland needs all of:
#   - a GNOME Wayland session;
#   - the DisplayXR GNOME Shell extension, with window drag (the plugin moves the
#     window through it, and the runtime weaves into the window through it);
#   - DisplayXR runtime 2.28.0 or newer (XR_DXR_wayland_surface_binding);
#   - a Vulkan loader.
# A transparent overlay app (DisplayXR Manifest Settings > Transparent Overlay
# App) also gets -popupwindow: no title bar or shadow around its transparent
# window. An opaque app keeps its title bar, the only way to move a window on a
# desktop without the extension.
#
# DISPLAYXR_LINUX_BACKEND=wayland or =x11 forces one. Arguments are passed on.

DIR=$(dirname "$(readlink -f "$0")")
PLAYER="$DIR/@PLAYER@"
SETTINGS="$DIR/@DATA@/DisplayXR/linux_player.json"

log() {
	echo "[DisplayXR launch] $*" >&2
}

# The DisplayXR GNOME extension's placement capabilities, empty if it is not running.
extension_caps() {
	if command -v gdbus >/dev/null 2>&1; then
		gdbus call --session --dest org.displayxr.WindowGeometry --object-path /org/displayxr/WindowPlacement \
			--method org.displayxr.WindowPlacement1.GetPlacementCapabilities 2>/dev/null |
			sed -n 's/^(uint32 \([0-9][0-9]*\),)$/\1/p'
	elif command -v busctl >/dev/null 2>&1; then
		busctl --user call org.displayxr.WindowGeometry /org/displayxr/WindowPlacement \
			org.displayxr.WindowPlacement1 GetPlacementCapabilities 2>/dev/null |
			sed -n 's/^u \([0-9][0-9]*\)$/\1/p'
	fi
}

have_vulkan_loader() {
	for d in /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib /lib/x86_64-linux-gnu /lib64; do
		[ -e "$d/libvulkan.so.1" ] && return 0
	done
	for ldconfig in ldconfig /sbin/ldconfig /usr/sbin/ldconfig; do
		if command -v "$ldconfig" >/dev/null 2>&1; then
			"$ldconfig" -p 2>/dev/null | grep -q 'libvulkan\.so\.1' && return 0
			return 1
		fi
	done
	return 1
}

# 0 when native Wayland can work; otherwise 1, with the reason in $why.
wayland_works() {
	if [ "${XDG_SESSION_TYPE:-}" != wayland ] || [ -z "${WAYLAND_DISPLAY:-}" ]; then
		why="not a Wayland session"
		return 1
	fi
	case ":${XDG_CURRENT_DESKTOP:-}:" in
	*:GNOME:*) ;;
	*)
		why="not GNOME (${XDG_CURRENT_DESKTOP:-unknown desktop})"
		return 1
		;;
	esac
	caps=$(extension_caps)
	if [ -z "$caps" ]; then
		why="the DisplayXR GNOME extension is not running"
		return 1
	fi
	if [ $((caps & 4)) -eq 0 ]; then
		why="the DisplayXR GNOME extension has no window drag (it needs version 9 or newer)"
		return 1
	fi
	version=$(dpkg-query -W -f='${Version}' displayxr-runtime 2>/dev/null)
	if [ -z "$version" ]; then
		why="the DisplayXR runtime's version cannot be told (no displayxr-runtime package)"
		return 1
	fi
	if ! dpkg --compare-versions "$version" ge 2.28.0; then
		why="DisplayXR runtime $version is older than 2.28.0"
		return 1
	fi
	if ! have_vulkan_loader; then
		why="no Vulkan loader (libvulkan.so.1)"
		return 1
	fi
	return 0
}

case "${DISPLAYXR_LINUX_BACKEND:-}" in
wayland)
	backend=wayland
	why="DISPLAYXR_LINUX_BACKEND=wayland"
	;;
x11)
	backend=x11
	why="DISPLAYXR_LINUX_BACKEND=x11"
	;;
*)
	if wayland_works; then
		backend=wayland
		why="GNOME Wayland session, DisplayXR extension, runtime $version"
	else
		backend=x11
	fi
	;;
esac

if [ "$backend" = wayland ]; then
	if grep -q '"transparent_overlay"[[:space:]]*:[[:space:]]*true' "$SETTINGS" 2>/dev/null; then
		set -- -popupwindow "$@"
	fi
	set -- -force-wayland "$@"
	log "native Wayland ($why)"
else
	log "X11 ($why)"
fi
exec "$PLAYER" "$@"
