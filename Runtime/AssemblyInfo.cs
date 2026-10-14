using System.Runtime.CompilerServices;

[assembly: InternalsVisibleTo("com.displayxr.unity.editor")]
// The render-pipeline sub-assemblies host the depth-aware cursor's depth hooks
// (DisplayXRCursorDepthPatch is internal plumbing, not app API).
[assembly: InternalsVisibleTo("com.displayxr.unity.urp")]
[assembly: InternalsVisibleTo("com.displayxr.unity.hdrp")]
