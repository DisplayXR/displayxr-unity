// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnityEditor;
using UnityEngine;
using UnityEngine.UI;

namespace DisplayXR.Editor
{
    /// <summary>
    /// Context menu of a legacy <c>InputField</c>: turn it into a
    /// <see cref="DisplayXRClipboardLegacyInputField"/> in place, keeping all its settings and
    /// references (the script reference is swapped; the subclass adds no serialized fields).
    /// Undoable. Not on a prefab instance: open the prefab.
    /// </summary>
    static class DisplayXRClipboardLegacyInputFieldMenu
    {
        const string k_Item = "CONTEXT/InputField/Copy and Paste Through DisplayXR";

        [MenuItem(k_Item)]
        static void Swap(MenuCommand command)
        {
            MonoScript script = FindScript(typeof(DisplayXRClipboardLegacyInputField));
            if (script == null)
            {
                Debug.LogError("[DisplayXR] DisplayXRClipboardLegacyInputField's script was not found");
                return;
            }
            var so = new SerializedObject(command.context);
            so.FindProperty("m_Script").objectReferenceValue = script;
            so.ApplyModifiedProperties();
        }

        [MenuItem(k_Item, true)]
        static bool CanSwap(MenuCommand command) =>
            command.context is InputField && !(command.context is DisplayXRClipboardLegacyInputField) &&
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
