// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System.Collections.Generic;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

namespace DisplayXR.Editor
{
    /// <summary>
    /// <b>GameObject &gt; DisplayXR &gt; Camera-Centric Rig / Display-Centric Rig</b> (#462).
    /// <para>
    /// On a selected Camera the item adds the rig component to it. With no Camera
    /// selected it creates a new GameObject holding a Camera and the rig, as a child of
    /// the selected object if there is one. Either way it is one Undo step, and every
    /// rig value is the component's own default - the menu sets none.
    /// </para>
    /// The same components are also under <b>Component &gt; DisplayXR</b> (their
    /// <c>AddComponentMenu</c>), which is the route when the Camera is already selected
    /// in the Inspector.
    /// </summary>
    static class DisplayXRRigMenu
    {
        // 10 puts the items in the creation block of the GameObject menu, which is also
        // what makes them show in the Hierarchy's right-click menu.
        const int Priority = 10;

        [MenuItem("GameObject/DisplayXR/Camera-Centric Rig", false, Priority)]
        static void CameraCentric(MenuCommand command)
        {
            AddOrCreate<DisplayXRCamera, DisplayXRDisplay>(command, "DisplayXR Camera Rig");
        }

        [MenuItem("GameObject/DisplayXR/Display-Centric Rig", false, Priority + 1)]
        static void DisplayCentric(MenuCommand command)
        {
            AddOrCreate<DisplayXRDisplay, DisplayXRCamera>(command, "DisplayXR Display Rig");
        }

        /// <typeparam name="TRig">The rig to add.</typeparam>
        /// <typeparam name="TOther">The other rig, which cannot share a GameObject with it.</typeparam>
        static void AddOrCreate<TRig, TOther>(MenuCommand command, string newName)
            where TRig : MonoBehaviour where TOther : MonoBehaviour
        {
            // From the Hierarchy's right-click menu Unity calls this once per selected
            // object, with that object as the context. From the main menu bar there is no
            // context and the selection is ours to read.
            var context = command.context as GameObject;
            var targets = context != null
                ? new[] { context }
                : Selection.gameObjects;

            var cameras = new List<GameObject>();
            foreach (var go in targets)
                if (go != null && !EditorUtility.IsPersistent(go) && go.GetComponent<Camera>() != null)
                    cameras.Add(go);

            string undoName = "Add " + ObjectNames.NicifyVariableName(typeof(TRig).Name);

            if (cameras.Count > 0)
            {
                foreach (var go in cameras)
                {
                    if (go.GetComponent<TRig>() != null)
                        continue; // already a rig of this kind
                    if (go.GetComponent<TOther>() != null)
                    {
                        // The two rigs both drive the camera's projection; the components
                        // themselves log an error when they meet (their OnValidate).
                        Debug.LogWarning("[DisplayXR] '" + go.name + "' already has a " + typeof(TOther).Name +
                                         ". A camera takes one rig: remove it first.", go);
                        continue;
                    }
                    Undo.AddComponent<TRig>(go);
                }
                if (context == null)
                    Selection.objects = cameras.ToArray();
                return;
            }

            // No camera in the selection: make one. ObjectFactory applies the project's
            // default presets, as Unity's own GameObject > Camera does.
            var parent = context != null ? context : Selection.activeGameObject;
            if (parent != null && EditorUtility.IsPersistent(parent))
                parent = null; // a prefab asset selected in the Project window

            var rig = ObjectFactory.CreateGameObject(newName, typeof(Camera), typeof(TRig));
            StageUtility.PlaceGameObjectInCurrentStage(rig);
            GameObjectUtility.SetParentAndAlign(rig, parent);
            GameObjectUtility.EnsureUniqueNameForSibling(rig);

            // A scene's first camera is its main one and carries its audio listener;
            // a second camera must not take either.
            if (Camera.main == null)
                rig.tag = "MainCamera";
            if (DisplayXRProjectValidation.FindInLoadedScenes<AudioListener>().Length == 0)
                ObjectFactory.AddComponent<AudioListener>(rig);

            Undo.RegisterCreatedObjectUndo(rig, undoName);
            Selection.activeGameObject = rig;
        }
    }
}
