// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

namespace DisplayXR.Editor
{
    /// <summary>
    /// <b>GameObject &gt; DisplayXR &gt; Camera-Centric Rig / Display-Centric Rig</b> (#462).
    /// <para>
    /// The item acts ONCE over the whole selection. If the selection holds cameras, it
    /// adds the rig component to each of them and creates nothing. If it holds none, it
    /// creates one new GameObject with a Camera and the rig, as a child of the selected
    /// object if there is one. Either way it is one Undo step, and every rig value is the
    /// component's own default - the menu sets none.
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
            // From the Hierarchy's right-click menu Unity calls this once PER selected
            // object, each time with that object as the context; from the main menu bar it
            // calls it once with no context. Act on the first of those calls only, and
            // over the whole selection - otherwise [Camera, Cube] would also create a new
            // rig under Cube, and N selected non-cameras would create N rigs.
            var context = command.context as GameObject;
            var selection = Selection.gameObjects;
            int contextIndex = context != null ? Array.IndexOf(selection, context) : -1;
            if (!DisplayXRMenuRules.ActsOnThisInvocation(context != null, contextIndex))
                return;
            var targets = DisplayXRMenuRules.CoversWholeSelection(context != null, contextIndex)
                ? selection
                : new[] { context };

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
                return;
            }

            // No camera in the selection: make ONE. ObjectFactory applies the project's
            // default presets, as Unity's own GameObject > Camera does.
            var parent = context != null ? context : Selection.activeGameObject;
            if (parent != null && EditorUtility.IsPersistent(parent))
                parent = null; // a prefab asset selected in the Project window

            var rig = ObjectFactory.CreateGameObject(newName, typeof(Camera), typeof(TRig));
            StageUtility.PlaceGameObjectInCurrentStage(rig);
            GameObjectUtility.SetParentAndAlign(rig, parent);
            GameObjectUtility.EnsureUniqueNameForSibling(rig);

            // A scene's first camera is its main one and carries its audio listener; a
            // second camera must not take either. In Prefab Mode neither question can be
            // answered: the prefab's own preview scene has no camera and no listener, but
            // the scenes the prefab will be placed in almost certainly do - so leave both
            // alone there and let whoever places the prefab decide.
            if (PrefabStageUtility.GetCurrentPrefabStage() == null)
            {
                if (Camera.main == null)
                    rig.tag = "MainCamera";
                if (DisplayXRProjectValidation.FindInLoadedScenes<AudioListener>().Length == 0)
                    ObjectFactory.AddComponent<AudioListener>(rig);
            }

            Undo.RegisterCreatedObjectUndo(rig, undoName);
            // Select the new rig AFTER this command: the remaining per-object invocations
            // of a multi-selection decide whether to act by looking at the selection, so
            // it must not change under them.
            EditorApplication.delayCall += () => { if (rig != null) Selection.activeGameObject = rig; };
        }
    }
}
