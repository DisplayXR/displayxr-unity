# SDL fixups (Linux, native Wayland)

Unity's Linux player links SDL2 statically into `UnityPlayer.so`, and Unity 6000.4.6f1's copy is a
2022 snapshot (after 2.0.22, before 2.24.0) that predates some upstream SDL Wayland fixes. Where
one of those bugs crashes a DisplayXR player, the plug-in applies the upstream fix from outside,
without changing Unity's files. The code is `native~/displayxr_linux_sdl_fixups.{h,c}`, in the main
plug-in `libdisplayxr_unity.so`.

## When it runs

Once, at the plug-in's earliest entry in a player: `XRSDKPreInit` (the boot.config pre-init load,
before Unity creates its graphics device and the player's window), else `UnityPluginLoad`. That is
before any monitor can come or go, and it needs no libwayland (it only edits tables inside
`UnityPlayer.so`), so the NEEDED lists stay as they are. It is installed in an X11 / XWayland player
too, where SDL never registers these Wayland listeners, so the swapped entry is never called. In the
editor (no `UnityPlayer.so`) nothing happens and nothing is logged. Windows and macOS never build it.

## How

SDL keeps one static, read-only listener table per Wayland interface in `UnityPlayer.so`'s RELRO,
and libwayland reads the table at every event it dispatches. A fix swaps one entry of such a table
for a wrapper (the page is made writable, one atomic pointer store, the page gets its previous
protection back, read from `/proc/self/maps`; no swap if that cannot be read) that does what the
upstream fix does and then calls SDL's own handler. Interposing `wl_*`
symbols could not do it: `UnityPlayer.so` dlopens libwayland and calls it through pointers it
looked up itself.

**Gating.** Nothing is touched unless `UnityPlayer.so`'s GNU build-id is in the build table
(`k_builds`), the table lies in its RELRO, its entries hold exactly the expected SDL handlers, and the
SDL code that reads every address and struct offset the wrapper uses matches byte for byte. Any
mismatch (another Unity version, a patched player) is a log line and the player runs exactly as
without the module. Before any swap the plug-in pins itself in memory (`RTLD_NODELETE`), so it is
never unloaded while SDL can call into it, even if the engine closes the handle it opened for
`XRSDKPreInit`; if the pin fails, nothing is installed.

## The fixes

| Fix | Symptom | Upstream | Issue |
|---|---|---|---|
| `clipboard` | A stock text field's Ctrl+C (a `GUIUtility.systemCopyBuffer` write) crashes the player after the app copied, another app copied, and the app pasted; with file drops on, a read (Ctrl+V) can crash too. SIGSEGV in `wl_proxy_marshal` from `UnityPlayer.so+0x26de032`. | SDL `a6179e85c` (2.24.0, SDL #6007) | #412 |
| `outputs` | A monitor removed and added again (GNOME Settings off then Revert, a cable blink, a dock re-enumerating) crashes the player: SIGSEGV at `UnityPlayer.so` vaddr `0x26d98c2`. | SDL `5f682e77c` (2.24.0) | #410 |

`clipboard`: when another client takes the clipboard, SDL gets `wl_data_source.cancelled` and frees
its source (`data_source_handle_cancelled` → `Wayland_data_source_destroy`) but leaves
`SDL_WaylandDataDevice::selection_source` pointing at it. The next `systemCopyBuffer` write destroys
that freed source again (a `wl_proxy_marshal` on freed memory — the crash); a read with no selection
offer walks its freed MIME list. The wrapper of SDL's `wl_data_source` `cancelled` clears
`selection_source` when it is the source being freed (the upstream code), then calls SDL's handler.
Its signatures include `SDL_SetClipboardText`'s read of SDL's `_this`, so it checks `_this` itself when
it is the only fix on. After the fix, stock fields and `GUIUtility.systemCopyBuffer` work on native
Wayland as they do on a desktop. With file drops on, after SDL's data device is cut (#398), a stock
field's paste still reads `""` or an older text — SDL has no clipboard offers then — so the DisplayXR
input fields (#394) are still the way to paste in that state. Unity's SDL snapshot has a second,
harmless clipboard bug: on a drag leaving the window `data_device_handle_leave` clears the selection
offer instead of the drag offer (fixed upstream only in SDL 2.30); with the use-after-free fixed it
can at most make a paste read `""` until the next selection change.

`outputs`: SDL frees a removed `wl_output`'s `SDL_WaylandOutputData` but leaves it linked in
`SDL_VideoData::output_list`, so the next `wl_output` that appears makes `Wayland_add_display` walk
into freed memory. The wrapper of SDL's `wl_registry` `global_remove` unlinks the node first (the
upstream code), then calls SDL's handler, which deletes the display and frees the data as before.

A plain Unity player without DisplayXR has these crashes too; the plug-in can only fix its own players.

## Log lines (stderr, the player log)

```
[DisplayXR-SDLFIX] Unity 6000.4.6f1 Linux x86_64 player (build-id 4ae6b3f4..., base 0x...): clipboard fixed, monitor re-add fixed
[DisplayXR-SDLFIX] another app took the clipboard: cleared SDL's selection source before SDL frees it
[DisplayXR-SDLFIX] monitor removed (wl_output global 50): unlinked SDL's output data before SDL frees it
```

A refusal says why, per fix (`clipboard fix NOT installed: SDL's code differs from the known build's`,
`monitor re-add fix NOT installed: SDL's wl_registry listener is not as expected`, ...), then the
summary (`... clipboard NOT fixed, monitor re-add fixed`); or `UnityPlayer.so build-id ... is not a known
build: nothing installed`.

## Switches

- `DISPLAYXR_SDL_FIXUPS=0` (or `off`): the kill switch, nothing installed; the player behaves as Unity
  ships it.
- Unset, empty, `1`, `on` or `all`: every fix.
- `DISPLAYXR_SDL_FIXUPS=clipboard,outputs`: a comma-separated list of exact fix names (blanks around a
  name are ignored), only those. A name the plug-in does not know is logged once and ignored.
- `DISPLAYXR_SDL_FIXUPS_LOG=1`: also log SDL's output list after every monitor removal.

## Adding a Unity build or a fix

A build entry needs the build-id (`readelf -n UnityPlayer.so`), the vaddrs of the listener table and
its handlers (`readelf -rW` shows the table's `R_X86_64_RELATIVE` addends), SDL's `_this`, the struct
offsets, and the code bytes that read them (`objdump -d`). The unit test
(`native~/tests/test_sdl_fixups.c`, CTest) checks the 6000.4.6f1 entry's addresses and offsets
against its own signatures; extend it for a new entry. A new fix gets a `DXR_SDLFIX_*` bit (in
`DXR_SDLFIX_ALL` too) and a name in `k_fix_names`, its fields and signatures in `struct dxr_sdl_build`, and an `install_<fix>()`
called from `install_with()` that verifies first and then swaps.
