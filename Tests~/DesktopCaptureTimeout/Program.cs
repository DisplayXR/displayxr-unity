// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for DisplayXRDesktopCaptureTimeout - how long a blocking desktop capture
// (DisplayXRDesktop.CaptureBlocking) waits: the main thread is capped lower than a worker.
// Hand-rolled (no test framework) so it also runs from a bare `csc` + the .NET runtime
// that ships inside the Unity editor. Exit code 0 = all passed.

using System;
using System.Threading;
using DisplayXR;

static class Program
{
    static int s_pass, s_fail;

    static void Check(bool ok, string what)
    {
        if (ok) s_pass++;
        else { s_fail++; Console.WriteLine("FAIL " + what); }
    }

    static void Expect(int timeoutMs, bool main, int want)
    {
        int got = DisplayXRDesktopCaptureTimeout.Budget(timeoutMs, main);
        Check(got == want, $"Budget({timeoutMs}, main={main}) = {got}, want {want}");
    }

    static void TestConstants()
    {
        Check(DisplayXRDesktopCaptureTimeout.DefaultMs == 5000, "default 5 s");
        Check(DisplayXRDesktopCaptureTimeout.MaxMs == 60000, "worker cap 60 s (unchanged)");
        Check(DisplayXRDesktopCaptureTimeout.MaxMainThreadMs == 10000, "main-thread cap 10 s");
        // The default must pass through on the main thread untouched, and the main cap is the lower one.
        Check(DisplayXRDesktopCaptureTimeout.DefaultMs <= DisplayXRDesktopCaptureTimeout.MaxMainThreadMs,
              "default <= main-thread cap");
        Check(DisplayXRDesktopCaptureTimeout.MaxMainThreadMs < DisplayXRDesktopCaptureTimeout.MaxMs,
              "main-thread cap < worker cap");
    }

    static void TestWorker()
    {
        // Off the main thread: exactly the behaviour before the main-thread cap.
        Expect(0, false, 0);
        Expect(1, false, 1);
        Expect(5000, false, 5000);
        Expect(10000, false, 10000);
        Expect(10001, false, 10001);
        Expect(30000, false, 30000);
        Expect(60000, false, 60000);
        Expect(60001, false, 60000);
        Expect(int.MaxValue, false, 60000);
        Expect(-1, false, 60000);
        Expect(int.MinValue, false, 60000);
    }

    static void TestMain()
    {
        Expect(0, true, 0); // a poll-only wait stays a poll-only wait
        Expect(1, true, 1);
        Expect(5000, true, 5000); // the default is not touched
        Expect(9999, true, 9999);
        Expect(10000, true, 10000);
        Expect(10001, true, 10000); // clamped, silently
        Expect(60000, true, 10000);
        Expect(int.MaxValue, true, 10000);
        Expect(-1, true, 10000); // "as long as allowed" = the main thread's cap
        Expect(int.MinValue, true, 10000);
    }

    static void TestInvariants()
    {
        // Every request: never above the caller's cap, never negative, and the request itself
        // whenever it fits.
        int[] probes = { int.MinValue, -60001, -1, 0, 1, 50, 4999, 5000, 5001, 9999, 10000, 10001, 59999, 60000,
                         60001, 600000, int.MaxValue };
        foreach (bool main in new[] { false, true })
        {
            int cap = main ? DisplayXRDesktopCaptureTimeout.MaxMainThreadMs : DisplayXRDesktopCaptureTimeout.MaxMs;
            foreach (int t in probes)
            {
                int b = DisplayXRDesktopCaptureTimeout.Budget(t, main);
                Check(b >= 0 && b <= cap, $"Budget({t}, main={main}) = {b} within 0..{cap}");
                if (t >= 0 && t <= cap) Check(b == t, $"Budget({t}, main={main}) keeps a request that fits");
                else Check(b == cap, $"Budget({t}, main={main}) is the cap");
            }
        }
    }

    // Which thread counts as the main one: the one recorded, nobody before it is recorded.
    static void TestThreads()
    {
        int OnWorker(Func<int> f)
        {
            int r = -42;
            var t = new Thread(() => r = f());
            t.Start();
            t.Join();
            return r;
        }

        // Not recorded yet (no RuntimeInitializeOnLoadMethod ran): every thread is a worker.
        Check(!DisplayXRDesktopCaptureTimeout.OnMainThread, "unrecorded: this thread is not the main one");
        Check(DisplayXRDesktopCaptureTimeout.BudgetForThisThread(60000) == 60000, "unrecorded: worker cap");

        DisplayXRDesktopCaptureTimeout.RecordMainThread();
        Check(DisplayXRDesktopCaptureTimeout.OnMainThread, "recorded: this thread is the main one");
        Check(DisplayXRDesktopCaptureTimeout.BudgetForThisThread(60000) == 10000, "main thread: capped at 10 s");
        Check(DisplayXRDesktopCaptureTimeout.BudgetForThisThread(-1) == 10000, "main thread: negative = 10 s");
        Check(DisplayXRDesktopCaptureTimeout.BudgetForThisThread(5000) == 5000, "main thread: default kept");

        Check(OnWorker(() => DisplayXRDesktopCaptureTimeout.OnMainThread ? 1 : 0) == 0, "worker is not the main one");
        Check(OnWorker(() => DisplayXRDesktopCaptureTimeout.BudgetForThisThread(60000)) == 60000,
              "worker thread: 60 s kept");
        Check(OnWorker(() => DisplayXRDesktopCaptureTimeout.BudgetForThisThread(-1)) == 60000,
              "worker thread: negative = 60 s");
        Check(OnWorker(() => DisplayXRDesktopCaptureTimeout.BudgetForThisThread(30000)) == 30000,
              "worker thread: 30 s kept");
    }

    static int Main()
    {
        TestConstants();
        TestWorker();
        TestMain();
        TestInvariants();
        TestThreads();
        Console.WriteLine($"DesktopCaptureTimeout: {s_pass} passed, {s_fail} failed");
        return s_fail == 0 ? 0 : 1;
    }
}
