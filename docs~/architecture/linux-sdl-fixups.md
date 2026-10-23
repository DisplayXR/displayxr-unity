# SDL fixups (Linux, native Wayland)

Unity's Linux player links SDL2 statically into `UnityPlayer.so`, and Unity 6000.4.6f1's copy is a
2022 snapshot (after 2.0.22, before 2.24.0) that predates some upstream SDL Wayland fixes. Where
one of those bugs crashes a DisplayXR player, the plug-in applies the upstream fix from outside,
without changing Unity's files. The same mechanism also records the IME's text-input events for the
DisplayXR input fields, which Unity's player itself loses (`ime`, below). The code is
`native~/displayxr_linux_sdl_fixups.{h,c}`, in the main plug-in `libdisplayxr_unity.so`.

## When it runs

Once, at the plug-in's earliest entry in a player: `XRSDKPreInit` (the boot.config pre-init load,
before Unity creates its graphics device and the player's window), else `UnityPluginLoad`. That is
before any monitor can come or go, and it needs no libwayland (it only edits tables inside
`UnityPlayer.so`), so the NEEDED lists stay as they are. Nothing in it depends on the DisplayXR
session or the plug-in's Wayland library: the fixes hold from the player's first frame. It is
installed in an X11 / XWayland player too, where SDL never registers these Wayland listeners, so the
swapped entries are never called. In the editor (no `UnityPlayer.so`) nothing happens and nothing is
logged. Windows and macOS never build it.

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
| `ime` | IME text (Korean, Chinese, ...) never reaches UGUI or TextMeshPro input fields; the Input System's `Keyboard.onTextInput` gets only the first character of each commit; the composition is never shown. | none: Unity's own event loop, not SDL | #415 |
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

`ime`: SDL turns `zwp_text_input_v3` `preedit_string` into `SDL_TEXTEDITING` and `commit_string`
into one `SDL_TEXTINPUT` carrying the whole commit; both are right. Unity's player loop then drops
`SDL_TEXTEDITING` (no composition, ever), hands an `SDL_TEXTINPUT` that is not paired with a key
press to the Input System as one text event holding only its first code point, and makes IMGUI
characters — the only text UGUI and TextMeshPro fields read — solely from key presses. An IME
commit is not a key press, so the fields get nothing and `onTextInput` gets `한` of `한글`. Nothing
upstream to copy. The fix wraps all six of SDL's text-input handlers and records the protocol's
double-buffered state at every `done` (composition with its cursor, commit, delete-around-caret).
While a DisplayXR input field (`DisplayXRClipboardInputField`, `DisplayXRClipboardLegacyInputField`)
has the keyboard focus it owns those events: they do not go on to SDL, so the text reaches the app
exactly once, through the field (`Runtime/DisplayXRTextIme.cs`, the plug-in's
`displayxr_linux_ime_*` exports). The field applies the frame's IME changes before the frame's keys,
inserts a commit through the base class's `Append` (validation, character limit, `onValueChanged`),
and shows the composition through the base class's own IME display (TextMeshPro: underlined; legacy
`InputField`: inline) via a `BaseInput` override on the input module. A Hangul Enter commits and
submits, as GTK does (the IME forwards the Enter after its commit). When the field gives the IME back
(deselect, the app losing the keyboard focus, disable) with a composition showing, the field keeps it
as text (or drops it on disable) and the IME is reset (`disable` / `enable` on SDL's text input, the
requests SDL itself sends). ibus-hangul answers the reset by committing that composition (its flush);
the field already took it, so that commit is dropped and never lands in the next field. Only the
flush is dropped: the IME's first commit after the reset, equal to the composition the field took,
before any new composition, other commit, delete or focus change (and within a second, a backstop);
an IME that does not flush loses nothing, as whatever it sends first disarms the drop. While a field
owns the IME, its caret (what it reports as `BaseInput.compositionCursorPos`) is sent as
`set_cursor_rectangle` for the IME's candidate window, and re-sent with the reset: Unity's player
never passes a text-input rectangle to SDL (it does not link `SDL_SetTextInputRect`). No field owning the
IME: every event goes on to SDL unchanged, so stock fields and `Keyboard.onTextInput` behave exactly
as Unity ships them. The reset looks `wl_proxy_marshal` up in the `libwayland-client` SDL already
loaded (`RTLD_NOLOAD`), so the main plug-in still links no libwayland. A field can take the IME only
once SDL's text input has had an event (the compositor's `enter` when the window gets the keyboard):
in an X11 player that never happens, and the fields stay their base class.

`outputs`: SDL frees a removed `wl_output`'s `SDL_WaylandOutputData` but leaves it linked in
`SDL_VideoData::output_list`, so the next `wl_output` that appears makes `Wayland_add_display` walk
into freed memory. The wrapper of SDL's `wl_registry` `global_remove` unlinks the node first (the
upstream code), then calls SDL's handler, which deletes the display and frees the data as before.

A plain Unity player without DisplayXR has these bugs too; the plug-in can only fix its own players,
and the IME text only in the DisplayXR input fields.

## Log lines (stderr, the player log)

```
[DisplayXR-SDLFIX] Unity 6000.4.6f1 Linux x86_64 player (build-id 4ae6b3f4..., base 0x...): clipboard fixed, IME for the DisplayXR fields, monitor re-add fixed
[DisplayXR-SDLFIX] another app took the clipboard: cleared SDL's selection source before SDL frees it
[DisplayXR-SDLFIX] monitor removed (wl_output global 50): unlinked SDL's output data before SDL frees it
```

With `DISPLAYXR_SDL_FIXUPS_LOG=1` the IME also logs `IME: reset (disable / enable)` (plus
`, cursor rectangle re-sent` when the field reported its caret) and `IME: dropped the IME's own commit
of a composition the field took itself`. Typed text never reaches a log.

A refusal says why, per fix (`clipboard fix NOT installed: SDL's code differs from the known build's`,
`IME fix NOT installed: SDL's zwp_text_input_v3 listener is not in UnityPlayer.so's RELRO`,
`monitor re-add fix NOT installed: SDL's wl_registry listener is not as expected`, ...), then the
summary (`... clipboard NOT fixed, IME for the DisplayXR fields, monitor re-add fixed`); or
`UnityPlayer.so build-id ... is not a known build: nothing installed`.

## Switches

- `DISPLAYXR_SDL_FIXUPS=0` (or `off`): the kill switch, nothing installed; the player behaves as Unity
  ships it.
- Unset, empty, `1`, `on` or `all`: every fix.
- `DISPLAYXR_SDL_FIXUPS=clipboard,outputs`: a comma-separated list of exact fix names (`clipboard`,
  `ime`, `outputs`; blanks around a name are ignored), only those. A name the plug-in does not know is
  logged once and ignored. Without `ime` the DisplayXR input fields type as stock fields do.
- `DISPLAYXR_SDL_FIXUPS_LOG=1`: also log SDL's output list after every monitor removal.

## Adding a Unity build or a fix

A build entry needs the build-id (`readelf -n UnityPlayer.so`), the vaddrs of the listener table and
its handlers (`readelf -rW` shows the table's `R_X86_64_RELATIVE` addends), SDL's `_this`, the struct
offsets, and the code bytes that read them (`objdump -d`). The unit test
(`native~/tests/test_sdl_fixups.c`, CTest) checks the 6000.4.6f1 entry's addresses and offsets
against its own signatures; extend it for a new entry. A new fix gets a `DXR_SDLFIX_*` bit (in
`DXR_SDLFIX_ALL` too) and a name in `k_fix_names`, its fields and signatures in `struct dxr_sdl_build`, and an `install_<fix>()`
called from `install_with()` that verifies first and then swaps.
