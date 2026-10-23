// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System.Reflection;
using TMPro;
using UnityEngine;
using UnityEngine.EventSystems;

namespace DisplayXR
{
    /// <summary>
    /// A TextMeshPro input field whose copy, cut and paste keep working in a Linux player that turned
    /// file drops on (<c>DisplayXRNative.displayxr_linux_file_drop_enable</c>). Use it instead of
    /// <c>TMP_InputField</c> (same settings and inspector; the context menu of a <c>TMP_InputField</c>
    /// swaps it in place).
    /// <para>Once the app has lost the focus with drops on, SDL's clipboard is cut: a write to
    /// <c>GUIUtility.systemCopyBuffer</c>, which <c>TMP_InputField</c> makes on Ctrl+C and Ctrl+X, can
    /// crash the player, and its Ctrl+V pastes stale text. From then on this field takes Ctrl+C,
    /// Ctrl+X and Ctrl+V itself and goes through the plug-in's clipboard
    /// (<c>DisplayXRNative.SetLinuxClipboardText</c>, <c>DisplayXRNative.LinuxClipboardText</c>):
    /// a copy puts the selection on the clipboard ("" with no selection and in a password field, as
    /// the base field); a cut also deletes it (not in a read-only field, and not when the copy failed);
    /// a paste reads the clipboard over the next frames without blocking one, then inserts the text at
    /// the caret, replacing the selection, only if the field still has the focus (nothing after about
    /// 3 s without an answer). It never touches <c>systemCopyBuffer</c> then.</para>
    /// <para>Before the cut, without file drops, on X11 / XWayland, on Windows and macOS and in the
    /// editor it is exactly a <c>TMP_InputField</c>. Clipboard text never reaches a log.</para>
    /// </summary>
    [AddComponentMenu("DisplayXR/DisplayXR Clipboard Input Field (TextMeshPro)")]
    public class DisplayXRClipboardInputField : TMP_InputField
    {
        // TMP_InputField.OnUpdateSelected is one loop over the frame's events, and the clipboard keys sit
        // inside KeyPressed, which is not virtual. Once the clipboard is cut this field runs that loop
        // itself with the clipboard keys taken out, and reaches the private state the loop keeps by
        // reflection so that every other key behaves exactly as in the base: the event instance (its
        // double-click check reads it), the IME composition flag (the keys that end a composition are
        // swallowed) and the text-mesh refresh flag. If a TextMeshPro version renamed one, the field
        // still routes the clipboard keys and logs once that IME edge cases may differ.
        const BindingFlags k_Private = BindingFlags.Instance | BindingFlags.NonPublic;
        static readonly FieldInfo s_ProcessingEvent = typeof(TMP_InputField).GetField("m_ProcessingEvent", k_Private);
        static readonly FieldInfo s_CompositionActive = typeof(TMP_InputField).GetField("m_IsCompositionActive", k_Private);
        static readonly FieldInfo s_TextUpdateRequired = typeof(TMP_InputField).GetField("m_IsTextComponentUpdateRequired", k_Private);
        static bool s_Checked;

        [System.NonSerialized] Event m_Event;
        [System.NonSerialized] int m_PastesPending;
        [System.NonSerialized] Coroutine m_Paste;

        public override void OnUpdateSelected(BaseEventData eventData)
        {
            if (!DisplayXRTextClipboard.Routes)
            {
                base.OnUpdateSelected(eventData);
                return;
            }
            if (!isFocused)
                return;
            CheckOnce();

            // TMP_InputField.OnUpdateSelected, line by line, but for the clipboard keys.
            Event evt = ProcessingEvent();
            bool consumedEvent = false;
            while (Event.PopEvent(evt))
            {
                EventType eventType = evt.rawType;
                if (eventType == EventType.KeyUp)
                    continue;

                if (eventType == EventType.KeyDown)
                {
                    consumedEvent = true;

                    // The base swallows the keys that end an IME composition.
                    if (CompositionActive() && CompositionLength() == 0 &&
                        evt.character == 0 && evt.modifiers == EventModifiers.None)
                        continue;

                    var key = DisplayXRTextClipboardRules.Classify((int)evt.modifiers, (int)evt.keyCode, false);
                    if (key != DisplayXRTextClipboardRules.Key.None)
                    {
                        ClipboardKey(key);
                    }
                    else if (KeyPressed(evt) == EditState.Finish)
                    {
                        if (!wasCanceled)
                            SendOnSubmit();
                        DeactivateInputField();
#if DISPLAYXR_TMP_32
                        break; // TextMeshPro 3.2 and newer (3.2.0-pre, 4.0.0-pre, the one in ugui 2.0+) drop the rest
#else
                        continue; // TextMeshPro 3.0.x goes on with the frame's other events
#endif
                    }

                    s_TextUpdateRequired?.SetValue(this, true);
                    UpdateLabel();
                    continue;
                }

                if ((eventType == EventType.ValidateCommand || eventType == EventType.ExecuteCommand) &&
                    evt.commandName == "SelectAll")
                {
                    SelectAll();
                    consumedEvent = true;
                }
            }

            if (consumedEvent)
            {
                UpdateLabel();
                eventData.Use();
            }
            else
            {
                // The queue is empty now, so this runs only the base's ending, which differs between
                // TextMeshPro versions (when the event is used, the IME label refresh).
                base.OnUpdateSelected(eventData);
            }
        }

        protected override void OnDisable()
        {
            if (m_Paste != null)
                StopCoroutine(m_Paste);
            m_Paste = null;
            m_PastesPending = 0;
            base.OnDisable();
        }

        void ClipboardKey(DisplayXRTextClipboardRules.Key key)
        {
            if (key == DisplayXRTextClipboardRules.Key.Paste)
            {
                if (readOnly)
                    return; // the base's Append does nothing in a read-only field
                m_PastesPending++;
                if (m_Paste == null)
                    m_Paste = StartCoroutine(DisplayXRTextClipboard.Read(Pasted, nameof(DisplayXRClipboardInputField)));
                return;
            }

            int anchor = stringPositionInternal, focus = stringSelectPositionInternal;
            string copy = DisplayXRTextClipboardRules.CopyText(text, anchor, focus, inputType == InputType.Password);
            bool copied = DisplayXRTextClipboard.Write(copy, nameof(DisplayXRClipboardInputField));
            // A cut deletes the selection as Backspace does (the same private steps as the base's cut).
            if (key == DisplayXRTextClipboardRules.Key.Cut && copied && anchor != focus)
                KeyPressed(Event.KeyboardEvent("backspace"));
        }

        void Pasted(string pasted)
        {
            int count = m_PastesPending;
            m_PastesPending = 0;
            m_Paste = null;
            if (pasted == null || !isFocused)
                return;
            for (int i = 0; i < count; i++)
                Append(pasted);
            s_TextUpdateRequired?.SetValue(this, true);
            UpdateLabel();
        }

        Event ProcessingEvent()
        {
            if (m_Event == null)
                m_Event = s_ProcessingEvent?.GetValue(this) as Event ?? new Event();
            return m_Event;
        }

        bool CompositionActive() => s_CompositionActive != null && (bool)s_CompositionActive.GetValue(this);

        // TMP_InputField.compositionLength (private).
        int CompositionLength()
        {
            if (readOnly)
                return 0;
            BaseInput input = EventSystem.current && EventSystem.current.currentInputModule
                ? EventSystem.current.currentInputModule.input : null;
            return (input != null ? input.compositionString : Input.compositionString).Length;
        }

        static void CheckOnce()
        {
            if (s_Checked)
                return;
            s_Checked = true;
            if (s_ProcessingEvent == null || s_CompositionActive == null || s_TextUpdateRequired == null)
                Debug.LogWarning("[DisplayXR] DisplayXRClipboardInputField: this TextMeshPro version's input field " +
                                 "differs from the one it was written for; copy, cut and paste still go through the " +
                                 "plug-in, the IME composition edge cases may differ from TMP_InputField");
        }
    }
}
