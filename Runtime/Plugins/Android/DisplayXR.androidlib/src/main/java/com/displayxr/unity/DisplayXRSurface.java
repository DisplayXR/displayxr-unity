// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Provider-owned weave surface for the DisplayXR IUnityXRDisplay provider on Android
// (displayxr-unity#418 P3b, design D3).
//
// WHAT THIS IS. The runtime weaves into a Surface the APP owns, handed over with
// XR_DXR_android_surface_binding. Unity's own SurfaceView cannot be that surface: one
// BufferQueue has exactly one producer, and Unity is already presenting into it. So the
// provider adds a SurfaceView of its own, stacked ABOVE Unity's, binds it at
// xrCreateSession and republishes it across surfaceDestroyed / surfaceCreated. The app
// (and this class) never draws into it; the runtime presents the weave there.
//
// The native side (libdisplayxr_unity.so, displayxr_android.cpp) calls attach() from
// Unity's graphics thread before xrCreateSession and registers the native methods
// below. Everything else here runs on the UI thread, and each fact the runtime needs is
// pushed from where Android reports it (runtime spec XR_DXR_android_surface_binding §4):
//
//   surfaceCreated / surfaceDestroyed -> xrSetAndroidSurfaceDXR(window / NULL)
//   Choreographer, every frame         -> xrSetAndroidWindowGeometryDXR, on change only
//   Window.Callback.dispatchTouchEvent -> xrSubmitAndroidInputEventDXR, one per pointer
//
// Z-ORDER. setZOrderMediaOverlay(true) puts this surface above every ordinary
// SurfaceView in the window (Unity's included) and below the window's own UI layer,
// which in a Unity player is empty. So Unity's surface can never flicker over the
// weave, whatever order the two surfaces are (re)created in. TextureView would composite
// through the view hierarchy and lose the direct SurfaceFlinger layer the weave's
// per-pixel interlacing needs — never use one here (spec §4).
//
// TOUCH. The view is neither clickable nor focusable, so touches fall through it to
// Unity's view exactly as if it were not there (the Input System / Input.touches keep
// working). A Window.Callback wrapper sees every MotionEvent first and hands a COPY to
// the runtime's touch input provider — submitting is a copy, never a consume (spec
// §2.7).

package com.displayxr.unity;

import android.app.Activity;
import android.app.Application;
import android.graphics.Point;
import android.os.Bundle;
import android.util.Log;
import android.view.ActionMode;
import android.view.Choreographer;
import android.view.Display;
import android.view.KeyEvent;
import android.view.Menu;
import android.view.MenuItem;
import android.view.MotionEvent;
import android.view.SearchEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.view.accessibility.AccessibilityEvent;
import android.widget.FrameLayout;

public final class DisplayXRSurface
        implements SurfaceHolder.Callback, Choreographer.FrameCallback,
                   Application.ActivityLifecycleCallbacks {

    private static final String TAG = "DisplayXRUnity";

    // Implemented in displayxr_android.cpp, registered with RegisterNatives by attach()'s
    // caller (the class is loaded through the app's class loader from a native thread).
    private static native void nativeSurfaceCreated(Surface surface, int width, int height);
    private static native void nativeSurfaceChanged(int width, int height);
    private static native void nativeSurfaceDestroyed();
    private static native void nativeWindowGeometry(int x, int y, int w, int h,
                                                    int panelW, int panelH, int displayId);
    private static native void nativeTouch(int action, int pointerId, int pointerCount,
                                           float x, float y, int surfaceW, int surfaceH,
                                           long eventTimeNs);
    private static native void nativeLifecycle(int resumed);

    // XrAndroidInputActionDXR
    private static final int XR_DOWN = 1, XR_MOVE = 2, XR_UP = 3, XR_CANCEL = 4;

    private static DisplayXRSurface sInstance;

    private final Activity mActivity;
    private SurfaceView mView;
    private boolean mFramePolling;
    private final int[] mLoc = new int[2];
    private final int[] mLocInWindow = new int[2];
    private final Point mReal = new Point();
    private final int[] mLastRect = new int[7];
    // Rotation ordering (#418 follow-up): frames a geometry sample has been held back
    // because the panel extent already reports the new rotation while the view has
    // not been laid out for it yet. Bounded by MAX_ROTATION_HOLD_FRAMES.
    private int mRotationHold;
    private static final int MAX_ROTATION_HOLD_FRAMES = 30; // 0.5 s at 60 Hz

    /**
     * Install the provider's SurfaceView over the activity's content. Idempotent; safe
     * from any thread (the work is posted to the UI thread).
     */
    public static void attach(final Activity activity) {
        if (activity == null) return;
        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                if (sInstance != null) return;
                sInstance = new DisplayXRSurface(activity);
                sInstance.install();
            }
        });
    }

    private DisplayXRSurface(Activity activity) {
        mActivity = activity;
        forgetRect();
    }

    private void forgetRect() {
        // Int.MIN so the first sample after (re)creation always differs and is pushed:
        // after a resume the runtime must be told the rect again even when it is
        // byte-identical to the one before (cube_handle_vk_android MainActivity.kt).
        for (int i = 0; i < mLastRect.length; i++) mLastRect[i] = Integer.MIN_VALUE;
    }

    private void install() {
        ViewGroup root = (ViewGroup) mActivity.findViewById(android.R.id.content);
        if (root == null) {
            Log.w(TAG, "[DisplayXR-AND] DisplayXRSurface: no content view — not installed");
            return;
        }
        mView = new SurfaceView(mActivity);
        mView.setZOrderMediaOverlay(true);
        mView.setFocusable(false);
        mView.setClickable(false);
        mView.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        mView.getHolder().addCallback(this);
        root.addView(mView, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        Window w = mActivity.getWindow();
        w.setCallback(new TouchTap(w.getCallback()));
        mActivity.getApplication().registerActivityLifecycleCallbacks(this);
        startPolling();
        Log.i(TAG, "[DisplayXR-AND] DisplayXRSurface installed over " + root.getClass().getSimpleName()
                + " (" + root.getChildCount() + " children, media-overlay z-order)");
    }

    // ------------------------------------------------------------ SurfaceHolder.Callback

    @Override public void surfaceCreated(SurfaceHolder holder) {
        android.graphics.Rect r = holder.getSurfaceFrame();
        nativeSurfaceCreated(holder.getSurface(), r.width(), r.height());
        forgetRect();
    }

    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        nativeSurfaceChanged(width, height);
        forgetRect();
    }

    @Override public void surfaceDestroyed(SurfaceHolder holder) {
        // Synchronous: the runtime must stop presenting into this window BEFORE it
        // dies, and it is only guaranteed alive until this callback returns.
        nativeSurfaceDestroyed();
    }

    // ------------------------------------------------------------ window geometry

    private void startPolling() {
        if (mFramePolling) return;
        mFramePolling = true;
        Choreographer.getInstance().postFrameCallback(this);
    }

    private void stopPolling() {
        mFramePolling = false;
        Choreographer.getInstance().removeFrameCallback(this);
    }

    // Choreographer rather than a layout / position listener: a pure window MOVE raises
    // no callback anywhere (WindowFrames.didFrameSizeChange compares w/h only), yet
    // SurfaceFlinger has already moved the layer — so sample every frame and push only
    // on a change. Cost: one getLocationOnScreen + seven int compares.
    @Override public void doFrame(long frameTimeNanos) {
        if (!mFramePolling) return;
        sampleRect();
        Choreographer.getInstance().postFrameCallback(this);
    }

    private void sampleRect() {
        if (mView == null) return;
        int w = mView.getWidth(), h = mView.getHeight();
        if (w <= 0 || h <= 0) return;
        Display d = mView.getDisplay();
        if (d == null) return;
        mView.getLocationOnScreen(mLoc);
        d.getRealSize(mReal); // the raw panel extent in the current rotation
        int id = d.getDisplayId();
        if (mLastRect[0] == mLoc[0] && mLastRect[1] == mLoc[1] && mLastRect[2] == w
                && mLastRect[3] == h && mLastRect[4] == mReal.x && mLastRect[5] == mReal.y
                && mLastRect[6] == id) return;
        // A rotation reaches the Display (getRealSize) one or more frames BEFORE the
        // view is re-laid out for it: for that window the sample pairs the NEW panel
        // extent with the OLD view rect, and the runtime would place / phase the weave
        // for a window that never existed (measured: one ~20 ms sample per rotation).
        // So when the panel extent changed but the view's size did not, hold the
        // sample until the layout catches up — bounded, because a window that really
        // keeps its size across a rotation (freeform) must still be reported.
        if (mLastRect[4] != Integer.MIN_VALUE && (mLastRect[4] != mReal.x || mLastRect[5] != mReal.y)
                && mLastRect[2] == w && mLastRect[3] == h && mRotationHold < MAX_ROTATION_HOLD_FRAMES) {
            if (mRotationHold++ == 0)
                Log.i(TAG, "[DisplayXR-AND] rotation: panel now " + mReal.x + "x" + mReal.y
                        + ", view still " + w + "x" + h + " — holding the geometry until it is re-laid out");
            return;
        }
        if (mRotationHold > 0) {
            Log.i(TAG, "[DisplayXR-AND] rotation: geometry released after " + mRotationHold
                    + " held frame(s): view " + w + "x" + h + " panel " + mReal.x + "x" + mReal.y);
            mRotationHold = 0;
        }
        mLastRect[0] = mLoc[0]; mLastRect[1] = mLoc[1]; mLastRect[2] = w; mLastRect[3] = h;
        mLastRect[4] = mReal.x; mLastRect[5] = mReal.y; mLastRect[6] = id;
        nativeWindowGeometry(mLoc[0], mLoc[1], w, h, mReal.x, mReal.y, id);
    }

    // ------------------------------------------------------------ lifecycle

    @Override public void onActivityResumed(Activity a) {
        if (a != mActivity) return;
        forgetRect(); // the surface was rebuilt underneath us: re-push even if unchanged
        startPolling();
        nativeLifecycle(1);
    }

    @Override public void onActivityPaused(Activity a) {
        if (a != mActivity) return;
        stopPolling();
        nativeLifecycle(0);
    }

    @Override public void onActivityCreated(Activity a, Bundle b) {}
    @Override public void onActivityStarted(Activity a) {}
    @Override public void onActivityStopped(Activity a) {}
    @Override public void onActivitySaveInstanceState(Activity a, Bundle b) {}
    @Override public void onActivityDestroyed(Activity a) {
        if (a != mActivity) return;
        stopPolling();
        mActivity.getApplication().unregisterActivityLifecycleCallbacks(this);
    }

    // ------------------------------------------------------------ touch

    private void submitTouch(MotionEvent ev) {
        if (mView == null) return;
        int sw = mView.getWidth(), sh = mView.getHeight();
        if (sw <= 0 || sh <= 0) return;
        // Window coordinates -> the bound surface's pixels.
        mView.getLocationInWindow(mLocInWindow);
        float ox = mLocInWindow[0], oy = mLocInWindow[1];
        long tNs = ev.getEventTime() * 1000000L; // uptime millis == CLOCK_MONOTONIC
        int am = ev.getActionMasked();
        int n = ev.getPointerCount();
        switch (am) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int i = ev.getActionIndex();
                nativeTouch(XR_DOWN, ev.getPointerId(i), n, ev.getX(i) - ox, ev.getY(i) - oy, sw, sh, tNs);
                break;
            }
            case MotionEvent.ACTION_MOVE:
                // ACTION_MOVE carries every finger: one event per pointer.
                for (int i = 0; i < n; i++)
                    nativeTouch(XR_MOVE, ev.getPointerId(i), n, ev.getX(i) - ox, ev.getY(i) - oy, sw, sh, tNs);
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                int i = ev.getActionIndex();
                nativeTouch(XR_UP, ev.getPointerId(i), n - 1, ev.getX(i) - ox, ev.getY(i) - oy, sw, sh, tNs);
                break;
            }
            case MotionEvent.ACTION_CANCEL:
                for (int i = 0; i < n; i++)
                    nativeTouch(XR_CANCEL, ev.getPointerId(i), 0, ev.getX(i) - ox, ev.getY(i) - oy, sw, sh, tNs);
                break;
            default:
                break;
        }
    }

    /** Sees every event before the activity does; delegates everything unchanged. */
    private final class TouchTap implements Window.Callback {
        private final Window.Callback mBase;

        TouchTap(Window.Callback base) { mBase = base; }

        @Override public boolean dispatchTouchEvent(MotionEvent event) {
            try {
                submitTouch(event);
            } catch (Throwable t) {
                // Never let the copy break the app's own input.
            }
            return mBase.dispatchTouchEvent(event);
        }

        @Override public boolean dispatchKeyEvent(KeyEvent e) { return mBase.dispatchKeyEvent(e); }
        @Override public boolean dispatchKeyShortcutEvent(KeyEvent e) { return mBase.dispatchKeyShortcutEvent(e); }
        @Override public boolean dispatchTrackballEvent(MotionEvent e) { return mBase.dispatchTrackballEvent(e); }
        @Override public boolean dispatchGenericMotionEvent(MotionEvent e) { return mBase.dispatchGenericMotionEvent(e); }
        @Override public boolean dispatchPopulateAccessibilityEvent(AccessibilityEvent e) { return mBase.dispatchPopulateAccessibilityEvent(e); }
        @Override public View onCreatePanelView(int f) { return mBase.onCreatePanelView(f); }
        @Override public boolean onCreatePanelMenu(int f, Menu m) { return mBase.onCreatePanelMenu(f, m); }
        @Override public boolean onPreparePanel(int f, View v, Menu m) { return mBase.onPreparePanel(f, v, m); }
        @Override public boolean onMenuOpened(int f, Menu m) { return mBase.onMenuOpened(f, m); }
        @Override public boolean onMenuItemSelected(int f, MenuItem i) { return mBase.onMenuItemSelected(f, i); }
        @Override public void onWindowAttributesChanged(WindowManager.LayoutParams a) { mBase.onWindowAttributesChanged(a); }
        @Override public void onContentChanged() { mBase.onContentChanged(); }
        @Override public void onWindowFocusChanged(boolean f) { mBase.onWindowFocusChanged(f); }
        @Override public void onAttachedToWindow() { mBase.onAttachedToWindow(); }
        @Override public void onDetachedFromWindow() { mBase.onDetachedFromWindow(); }
        @Override public void onPanelClosed(int f, Menu m) { mBase.onPanelClosed(f, m); }
        @Override public boolean onSearchRequested() { return mBase.onSearchRequested(); }
        @Override public boolean onSearchRequested(SearchEvent e) { return mBase.onSearchRequested(e); }
        @Override public ActionMode onWindowStartingActionMode(ActionMode.Callback c) { return mBase.onWindowStartingActionMode(c); }
        @Override public ActionMode onWindowStartingActionMode(ActionMode.Callback c, int t) { return mBase.onWindowStartingActionMode(c, t); }
        @Override public void onActionModeStarted(ActionMode m) { mBase.onActionModeStarted(m); }
        @Override public void onActionModeFinished(ActionMode m) { mBase.onActionModeFinished(m); }
        @Override public void onPointerCaptureChanged(boolean c) { mBase.onPointerCaptureChanged(c); }
        @Override public void onProvideKeyboardShortcuts(java.util.List<android.view.KeyboardShortcutGroup> d, Menu m, int id) { mBase.onProvideKeyboardShortcuts(d, m, id); }
    }
}
