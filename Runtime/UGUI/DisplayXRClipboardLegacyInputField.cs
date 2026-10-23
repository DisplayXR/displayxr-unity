// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System.Reflection;
using UnityEngine;
using UnityEngine.EventSystems;
using UnityEngine.UI;

namespace DisplayXR
{
    /// <summary>
    /// UGUI's legacy <c>InputField</c> with copy, cut and paste that keep working in a Linux player
    /// that turned file drops on: the legacy twin of <c>DisplayXRClipboardInputField</c>
    /// (TextMeshPro), which describes the behaviour. Use it instead of <c>InputField</c> (same settings
    /// and inspector; the context menu of an <c>InputField</c> swaps it in place).
    /// <para>Once SDL's clipboard is cut it takes Ctrl+C, Ctrl+X and Ctrl+V, and also the legacy
    /// field's Ctrl+Insert (copy) and Shift+Insert (paste), and goes through the plug-in's clipboard,
    /// never through <c>GUIUtility.systemCopyBuffer</c>. Before the cut, without file drops, on X11 /
    /// XWayland, on Windows and macOS and in the editor it is exactly an <c>InputField</c> for the
    /// clipboard. Clipboard text never reaches a log.</para>
    /// <para>In a native-Wayland Linux player it also gets IME text, which Unity's player loses there
    /// (#415): as <c>DisplayXRClipboardInputField</c>, with the composition shown inline.</para>
    /// </summary>
    [AddComponentMenu("DisplayXR/DisplayXR Clipboard Input Field (Legacy)")]
    public class DisplayXRClipboardLegacyInputField : InputField
    {
        // As in DisplayXRClipboardInputField: InputField.OnUpdateSelected is run here with the clipboard
        // keys taken out, and the private state its loop keeps (the event instance, the IME composition
        // flag) is reached by reflection, so every other key behaves exactly as in the base.
        const BindingFlags k_Private = BindingFlags.Instance | BindingFlags.NonPublic;
        static readonly FieldInfo s_ProcessingEvent = typeof(InputField).GetField("m_ProcessingEvent", k_Private);
        static readonly FieldInfo s_CompositionActive = typeof(InputField).GetField("m_IsCompositionActive", k_Private);
        static bool s_Checked;

        [System.NonSerialized] Event m_Event;
        [System.NonSerialized] int m_PastesPending;
        [System.NonSerialized] Coroutine m_Paste;

        public override void OnUpdateSelected(BaseEventData eventData)
        {
            ImeBeforeKeys();
            UpdateSelected(eventData);
            if (!isFocused)
                ImeRelease(false);
        }

        void UpdateSelected(BaseEventData eventData)
        {
            if (!DisplayXRTextClipboard.Routes)
            {
                base.OnUpdateSelected(eventData);
                return;
            }
            if (!isFocused)
                return;
            CheckOnce();

            // InputField.OnUpdateSelected, line by line, but for the clipboard keys.
            Event evt = ProcessingEvent();
            bool consumedEvent = false;
            while (Event.PopEvent(evt))
            {
                if (evt.rawType == EventType.KeyDown)
                {
                    consumedEvent = true;

                    // The base swallows the keys that end an IME composition.
                    if (CompositionActive() && CompositionString().Length == 0 &&
                        evt.character == 0 && evt.modifiers == EventModifiers.None)
                        continue;

                    var key = DisplayXRTextClipboardRules.Classify((int)evt.modifiers, (int)evt.keyCode, true);
                    if (key != DisplayXRTextClipboardRules.Key.None)
                    {
                        ClipboardKey(key);
                    }
                    else if (KeyPressed(evt) == EditState.Finish)
                    {
                        if (!wasCanceled)
                            SendOnSubmit();
                        DeactivateInputField();
                        continue;
                    }
                    UpdateLabel();
                }

                switch (evt.type)
                {
                    case EventType.ValidateCommand:
                    case EventType.ExecuteCommand:
                        if (evt.commandName == "SelectAll")
                        {
                            SelectAll();
                            consumedEvent = true;
                        }
                        break;
                }
            }

            if (consumedEvent)
                UpdateLabel();
            eventData.Use();
        }

        public override void OnDeselect(BaseEventData eventData)
        {
            ImeRelease(true); // a composition still showing becomes text
            base.OnDeselect(eventData);
        }

        // The app lost the keyboard focus: the compositor takes the IME away (the IME's own commit of the
        // composition does not reach the app then), so the composition still showing becomes text here.
        void OnApplicationFocus(bool hasFocus)
        {
            if (!hasFocus)
                ImeRelease(true);
        }

        protected override void LateUpdate()
        {
            base.LateUpdate();
            if (!isFocused)
                ImeRelease(true);
        }

        protected override void OnDisable()
        {
            ImeRelease(false);
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
                    m_Paste = StartCoroutine(DisplayXRTextClipboard.Read(Pasted, nameof(DisplayXRClipboardLegacyInputField)));
                return;
            }

            int anchor = caretPositionInternal, focus = caretSelectPositionInternal;
            string copy = DisplayXRTextClipboardRules.CopyText(text, anchor, focus, inputType == InputType.Password);
            bool copied = DisplayXRTextClipboard.Write(copy, nameof(DisplayXRClipboardLegacyInputField));
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
            UpdateLabel();
        }

        // IME: as in DisplayXRClipboardInputField (DisplayXRTextIme).
        void ImeBeforeKeys()
        {
            if (!isFocused || readOnly)
                return;
            if (!DisplayXRTextIme.Owns(this) && !DisplayXRTextIme.Acquire(this))
                return;
            bool changed = false;
            while (DisplayXRTextIme.Next(this, out DisplayXRTextIme.Change change))
            {
                changed = true;
                DisplayXRTextIme.SetComposition(this, "");
                if (change.DeleteBeforeBytes > 0 || change.DeleteAfterBytes > 0)
                    ImeDelete(change.DeleteBeforeBytes, change.DeleteAfterBytes);
                if (!string.IsNullOrEmpty(change.Commit))
                    Append(change.Commit);
                DisplayXRTextIme.SetComposition(this, change.Composition);
            }
            if (changed)
                UpdateLabel();
        }

        void ImeDelete(int beforeBytes, int afterBytes)
        {
            int caret = caretPositionInternal;
            int before = DisplayXRTextIme.CharsForBytes(text, caret, beforeBytes, true);
            int after = DisplayXRTextIme.CharsForBytes(text, caret, afterBytes, false);
            if (before + after == 0)
                return;
            caretPositionInternal = caret - before;
            caretSelectPositionInternal = caret + after;
            KeyPressed(Event.KeyboardEvent("backspace"));
        }

        void ImeRelease(bool commitComposition)
        {
            if (!DisplayXRTextIme.Owns(this))
                return;
            string composition = DisplayXRTextIme.Composition;
            DisplayXRTextIme.SetComposition(this, "");
            if (commitComposition && composition.Length > 0 && !readOnly)
                Append(composition);
            DisplayXRTextIme.Release(this);
            if (isActiveAndEnabled)
                UpdateLabel();
        }

        Event ProcessingEvent()
        {
            if (m_Event == null)
                m_Event = s_ProcessingEvent?.GetValue(this) as Event ?? new Event();
            return m_Event;
        }

        bool CompositionActive() => s_CompositionActive != null && (bool)s_CompositionActive.GetValue(this);

        // InputField.compositionString (private).
        static string CompositionString()
        {
            BaseInput input = EventSystem.current && EventSystem.current.currentInputModule
                ? EventSystem.current.currentInputModule.input : null;
            return input != null ? input.compositionString : Input.compositionString;
        }

        static void CheckOnce()
        {
            if (s_Checked)
                return;
            s_Checked = true;
            if (s_ProcessingEvent == null || s_CompositionActive == null)
                Debug.LogWarning("[DisplayXR] DisplayXRClipboardLegacyInputField: this UGUI version's InputField " +
                                 "differs from the one it was written for; copy, cut and paste still go through the " +
                                 "plug-in, the IME composition edge cases may differ from InputField");
        }
    }
}
