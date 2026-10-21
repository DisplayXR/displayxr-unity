// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System.Threading;

namespace DisplayXR
{
    /// <summary>
    /// How long a blocking capture (<c>DisplayXRDesktop.CaptureBlocking</c> and its siblings) waits.
    /// No UnityEngine types on purpose: it is unit-tested outside Unity
    /// (<c>Tests~/DesktopCaptureTimeout</c>, run in CI). The public constants are
    /// <c>DisplayXRDesktop.DefaultCaptureTimeoutMs</c>, <c>MaxCaptureTimeoutMs</c> and
    /// <c>MaxMainThreadCaptureTimeoutMs</c>.
    ///
    /// <para>The main thread gets a lower cap because a blocked main thread is a frozen frame: no
    /// rendering, no input, and past about 5 s the desktop may flag the window as not responding.
    /// The runtime bounds a capture itself (a new stream's first frame is awaited at most 2 s, then
    /// the capture fails), so a healthy one ends well inside the cap even with the 250 ms start
    /// spacing, several monitors and a large downscale; a longer wait only stretches the freeze. A
    /// worker thread freezes nothing and keeps the full cap.</para>
    /// </summary>
    internal static class DisplayXRDesktopCaptureTimeout
    {
        public const int DefaultMs = 5000;
        public const int MaxMs = 60000;
        public const int MaxMainThreadMs = 10000;

        // Unity's main thread (managed id), recorded by DisplayXRDesktop at startup. 0 = not recorded:
        // every thread is then treated as a worker (the full cap).
        private static int s_MainThreadId;

        public static void RecordMainThread() => s_MainThreadId = Thread.CurrentThread.ManagedThreadId;

        public static bool OnMainThread =>
            s_MainThreadId != 0 && Thread.CurrentThread.ManagedThreadId == s_MainThreadId;

        /// <summary>The wait budget in ms for a requested <paramref name="timeoutMs"/>: the request itself
        /// when it is in 0..cap, else the cap (a negative request means "as long as allowed"). The cap is
        /// <see cref="MaxMainThreadMs"/> on the main thread and <see cref="MaxMs"/> elsewhere.</summary>
        public static int Budget(int timeoutMs, bool onMainThread)
        {
            int cap = onMainThread ? MaxMainThreadMs : MaxMs;
            return timeoutMs < 0 || timeoutMs > cap ? cap : timeoutMs;
        }

        /// <summary><see cref="Budget"/> for the calling thread.</summary>
        public static int BudgetForThisThread(int timeoutMs) => Budget(timeoutMs, OnMainThread);
    }
}
