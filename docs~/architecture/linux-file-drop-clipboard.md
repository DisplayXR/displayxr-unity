# File drops and the clipboard (Linux, native Wayland)

A Linux player running natively on Wayland can receive files dragged onto its window (#395).
Turning that on changes what Unity's clipboard can do for the rest of the run (#398), and the
text fields have to follow (#394). This page is the app developer's view of all three.

Linux only: a player running natively on Wayland (`-force-wayland`, what the generated launcher
picks). X11 / XWayland players, Windows, macOS and the editor are not affected.

## File drops

```csharp
DisplayXRNative.displayxr_linux_file_drop_enable(1);   // 1 on now, 0 remembered until the session, -1 unsupported

// every frame, any thread:
while (DisplayXRNative.LinuxFileDropNext(out var ev, out string[] paths))
{
    // ev.kind: LinuxFileDropEnter (paths), LinuxFileDropMove, LinuxFileDropLeave, LinuxFileDropDrop (paths)
    // ev.windowX / windowY: the pointer in the window (device px); ev.x / y and ev.stageX / stageY when flagged
}
```

Only local files are offered, and a drop is accepted as COPY only (the file manager never
deletes the source). The full contract is on the API's doc comments and in the CHANGELOG
(2.30.2, 2.31.0).

## What drops do to Unity's clipboard

GNOME's compositor sends a drag to one Wayland data device per app. To receive drops, the
plug-in's own data device has to be that one, and making it so unlinks SDL's, which is the one
`GUIUtility.systemCopyBuffer` talks through. The plug-in does that at the app's **first focus
loss with drops on** (at once if drops are turned on while the app is not focused). From then on,
for the rest of the run ("SDL's clipboard is cut"):

- `GUIUtility.systemCopyBuffer` reads stale text: `""`, or an older text SDL still holds.
- **A write to `GUIUtility.systemCopyBuffer` can crash the player** inside SDL.
- Turning drops off does not bring SDL's clipboard back.

`DisplayXRNative.LinuxClipboardState()` tells which state the app is in:

| Bit | Constant | Meaning |
|---|---|---|
| 1 | `LinuxClipboardStateAvailable` | the plug-in's clipboard serves reads and writes |
| 2 | `LinuxClipboardStateFocused` | the app has the keyboard focus |
| 4 | `LinuxClipboardStateSdlCut` | SDL's clipboard is cut (above) |
| 8 | `LinuxClipboardStateOwned` | the clipboard holds the plug-in's last write |

Once it is cut, use the plug-in's clipboard: `DisplayXRNative.SetLinuxClipboardText(text)` and
`DisplayXRNative.LinuxClipboardText(out text)`. Both work only while the app has the keyboard
focus (Wayland's rule; without it, the runtime's `DisplayXRDesktop.GetClipboardText` /
`SetClipboardText` under the user's consent). On Unity's main thread a read never blocks: it
answers `Pending` until the text is in, so call it again next frame (the plug-in ends a read
after about 3 s). Before the cut both answer `UseSystemCopyBuffer`, and `systemCopyBuffer` is
still the right clipboard.

## Text fields

Unity's `TMP_InputField` and the legacy `InputField` copy and paste through
`systemCopyBuffer`: Ctrl+C and Ctrl+X write it, Ctrl+V reads it (the legacy field also copies on
Ctrl+Insert and pastes on Shift+Insert). In an app with drops on, a user who copies in a text
field after the cut can crash the player, and a paste brings stale text. Use the DisplayXR
fields instead:

| Instead of | Use | Compiled when the project has |
|---|---|---|
| `TMP_InputField` | `DisplayXRClipboardInputField` | TextMeshPro: `com.unity.ugui` 2.0+ (Unity 6) or `com.unity.textmeshpro` 3.0+ (2022.3) |
| `InputField` (legacy) | `DisplayXRClipboardLegacyInputField` | `com.unity.ugui` |

They are subclasses with no settings of their own, so the inspector and every setting stay the
same. To switch an existing field, use **Copy and Paste Through DisplayXR** in the context menu
of its `TMP_InputField` / `InputField` component: it swaps the component's script in place and
keeps all its settings and references (undoable; not offered on a prefab instance, so open the
prefab). Or add the component in place of the base one, in the editor or with `AddComponent`.

What they do once SDL's clipboard is cut (they check `LinuxClipboardState()` on every update
while focused):

| Key | Base field | DisplayXR field |
|---|---|---|
| Ctrl+C (legacy: also Ctrl+Insert) | selection to `systemCopyBuffer` | selection to `SetLinuxClipboardText` |
| Ctrl+X | the same, then deletes the selection | the same through the plug-in; deletes the selection only if the copy was done |
| Ctrl+V (legacy: also Shift+Insert) | inserts `systemCopyBuffer` | reads `LinuxClipboardText` over the next frames (never blocks one), then inserts at the caret, replacing the selection, if the field still has the focus |

- As in the base field, a copy without a selection or in a password field puts `""` on the
  clipboard (the password never), a read-only field copies but never changes, and a paste goes
  through the field's validation and character limit.
- A paste that gets no text (the read failed or nothing came in about 3 s) pastes nothing and
  logs one line naming the result. Clipboard text never reaches a log.
- **Every other key is the base field's own handling.** The base fields handle keys in one loop
  over the frame's events, with the clipboard keys inside a method that cannot be overridden, so
  the DisplayXR fields run that loop themselves with the clipboard keys taken out. They reach the
  private state the loop keeps by reflection (the event instance, the IME composition flag, and
  TextMeshPro's text refresh flag), so IME composition, Enter / Escape, multi-line, validation and
  read-only behave exactly as in the base. If a later UGUI or TextMeshPro version renames one of
  those, the field still routes the clipboard keys and logs once that IME edge cases may differ.
- Before the cut, without file drops, in an X11 / XWayland player, on Windows and macOS and in
  the editor they are exactly the base field: they call the base for everything.

Other text widgets that copy or paste through `systemCopyBuffer` (UI Toolkit, IMGUI, an app's own
fields) are not covered: route their copy and paste the same way, with `LinuxClipboardState()`,
`SetLinuxClipboardText` and `LinuxClipboardText`.

The rules (when to route, which keys, what a copy takes) are pure C#,
`Runtime/DisplayXRTextClipboardRules.cs`, unit-tested in CI (`Tests~/TextClipboard`).
