# SDL fixups (Linux, native Wayland)

Unity's Linux player links SDL2 statically into `UnityPlayer.so`, and Unity 6000.4.6f1's copy is a
2022 snapshot (after 2.0.22, before 2.24.0) that predates some upstream SDL Wayland fixes. Where
one of those bugs crashes a DisplayXR player, the plug-in applies the upstream fix from outside,
without changing Unity's files. The code is `native~/displayxr_linux_sdl_fixups.{h,c}`, part of the
Wayland support library `libdisplayxr_unity_wayland.so`.

## When it runs

Only in a player running natively on Wayland: the main plug-in loads the Wayland library when it
creates the first weave (at the start of the XR session), and the library's init
(`dxr_wl_lib_init`) installs the fixups once. X11 / XWayland players, the editor, Windows and macOS
never load it.

## How

SDL keeps one static, read-only listener table per Wayland interface in `UnityPlayer.so`'s RELRO,
and libwayland reads the table at every event it dispatches. A fix swaps one entry of such a table
for a wrapper (the page is made writable, one atomic pointer store, the page is made read-only
again) that does what the upstream fix does and then calls SDL's own handler. Interposing `wl_*`
symbols could not do it: `UnityPlayer.so` dlopens libwayland and calls it through pointers it
looked up itself.

**Gating.** Nothing is touched unless `UnityPlayer.so`'s GNU build-id is in the build table
(`dxr_sdl_builds`), the table lies in its RELRO, the table entries hold exactly the expected SDL
handlers, and the SDL code that reads every address and struct offset the wrapper uses matches byte
for byte. Any mismatch (another Unity version, a patched player) is one log line and the player
runs exactly as without the module. Once a wrapper is installed the library pins itself, so it is
never unloaded while SDL can call into it.

## The fixes

| Fix | Symptom | Upstream | Issue |
|---|---|---|---|
| `outputs` | A monitor removed and added again (GNOME Settings off then Revert, a cable blink, a dock re-enumerating) crashes the player: SIGSEGV at `UnityPlayer.so` vaddr `0x26d98c2`. | SDL `5f682e77c` (2.24.0) | #410 |

`outputs`: SDL frees a removed `wl_output`'s `SDL_WaylandOutputData` but leaves it linked in
`SDL_VideoData::output_list`, so the next `wl_output` that appears makes `Wayland_add_display` walk
into freed memory. The wrapper of SDL's `wl_registry` `global_remove` unlinks the node first (the
upstream code), then calls SDL's handler, which deletes the display and frees the data as before.
A plain Unity player without DisplayXR has the same crash; the plug-in can only fix its own players.

## Log lines (stderr, the player log)

```
[DisplayXR-WL] SDL fixups: Unity 6000.4.6f1 Linux x86_64 player (UnityPlayer.so build-id 4ae6b3f4..., base 0x...): monitor re-add fix installed
[DisplayXR-WL] SDL fixups: monitor removed (wl_output global 50): unlinked SDL's output data before SDL frees it
```

A refusal says why (`NOT installed (SDL's code differs from the known build's)`, `... is not a
known build: nothing installed`).

## Switches

- `DXR_SDLFIX=off` (or `0`): the kill switch, nothing installed; the player behaves as Unity ships it.
- `DXR_SDLFIX=outputs`: a comma list of fixes to install, only those (unset = all).
- `DXR_SDLFIX_VERBOSE=1`: also log SDL's output list after every monitor removal.

## Limits

- A monitor removed **before** the Wayland library loads (before the XR session starts) has already
  left a freed node in SDL's list; the fix cannot repair that, only every removal after it.
- Only the Unity builds in the table are fixed. Another Unity version gets nothing until its entry
  is added.

## Adding a Unity build or a fix

A build entry needs the build-id (`readelf -n UnityPlayer.so`), the vaddrs of the listener table and
its handlers (`readelf -rW` shows the table's `R_X86_64_RELATIVE` addends), SDL's `_this`, the struct
offsets, and the code bytes that read them (`objdump -d`). The unit test
(`native~/tests/test_sdl_fixups.c`, CTest) checks the 6000.4.6f1 entry's addresses and offsets
against its own signatures; extend it for a new entry. A new fix gets a `DXR_SDLFIX_*` bit and name,
its fields in `struct dxr_sdl_build`, and an `install_<fix>()` that verifies first and then swaps.
