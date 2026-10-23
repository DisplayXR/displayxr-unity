// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using TMPro;
using UnityEditor;
using UnityEngine;

namespace DisplayXR.Editor
{
    /// <summary>
    /// Context menu of a <c>TMP_InputField</c>: turn it into a <see cref="DisplayXRClipboardInputField"/>
    /// in place, keeping all its settings and references (the script reference is swapped; the
    /// subclass adds no serialized fields). Undoable. Not on a prefab instance: open the prefab.
    /// </summary>
    static class DisplayXRClipboardInputFieldMenu
    {
        const string k_Item = "CONTEXT/TMP_InputField/Copy and Paste Through DisplayXR";

        [MenuItem(k_Item)]
        static void Swap(MenuCommand command)
        {
            MonoScript script = FindScript(typeof(DisplayXRClipboardInputField));
            if (script == null)
            {
                Debug.LogError("[DisplayXR] DisplayXRClipboardInputField's script was not found");
                return;
            }
            var so = new SerializedObject(command.context);
            so.FindProperty("m_Script").objectReferenceValue = script;
            so.ApplyModifiedProperties();
        }

        [MenuItem(k_Item, true)]
        static bool CanSwap(MenuCommand command) =>
            command.context is TMP_InputField && !(command.context is DisplayXRClipboardInputField) &&
            !PrefabUtility.IsPartOfPrefabInstance(command.context);

        static MonoScript FindScript(System.Type type)
        {
            foreach (MonoScript s in MonoImporter.GetAllRuntimeMonoScripts())
                if (s != null && s.GetClass() == type)
                    return s;
            return null;
        }
    }
}
