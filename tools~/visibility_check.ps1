# Pre-release visibility check (DisplayXR/displayxr-unity#295, #296).
#
# The property under test is the one the #277 pre-cloak broke and CI + a 3D-path
# panel check both MISSED, twice: CAN A USER SEE THE APP? A cloaked, off-screen
# window still runs, presents, and pumps - it passes every "did it start" assertion.
# So this asserts the STRONGER property: a window that is simultaneously
#   visible  AND  not cloaked (DWMWA_CLOAKED)  AND  >200x200  AND  on-screen
# and, when -GuardLog is given, that the plugin's OWN #295 guard produced it
# (the 6th conjunct - an app may carry its own IsInstalled guard for this very
# bug and pass the arm while the plugin guard never ran; assert the plugin's line).
#
# Usage:
#   visibility_check.ps1 -Exe <player.exe> [-Seconds 25] [-NoRuntime] [-GuardLog <Player.log>]  (the #295 guard is C#: %USERPROFILE%\AppData\LocalLow\<Company>\<Product>\Player.log)
# -NoRuntime points XR_RUNTIME_JSON at a nonexistent path (the customer's config).
# Exit 0 = shown; exit 1 = INVISIBLE (the regression).
param(
  [Parameter(Mandatory=$true)][string]$Exe,
  [int]$Seconds = 25,
  [switch]$NoRuntime,
  [string]$GuardLog = ""
)
Add-Type @"
using System;using System.Runtime.InteropServices;using System.Text;
public class VZ {
 [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr p);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
 [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h,int a,out int v,int s);
 [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
 public delegate bool EnumProc(IntPtr h, IntPtr p);
 public struct RECT { public int L,T,R,B; }
 // Enumerate ALL pids named the same (a re-exec - GPU pin / API fallback - moves the
 // visible window to a SECOND process; single-PID tracking passes for the wrong reason).
 public static string Shown(int[] pids) {
   var sb = new StringBuilder();
   EnumWindows((h,p) => {
     uint pid; GetWindowThreadProcessId(h, out pid);
     bool mine=false; foreach(var q in pids){ if(q==(int)pid){mine=true;break;} }
     if (mine && IsWindowVisible(h)) {
       int cloaked=0; DwmGetWindowAttribute(h,14,out cloaked,4);
       RECT r; GetWindowRect(h,out r); int w=r.R-r.L, ht=r.B-r.T;
       if (cloaked==0 && w>200 && ht>200 && r.L>-30000 && r.T>-30000)
         sb.Append(w+"x"+ht+"@"+r.L+","+r.T+";");
     } return true; }, IntPtr.Zero);
   return sb.ToString(); }
 // Every top-level window of the pids, with its state - so a FAIL says WHICH conjunct
 // failed (a 288x150 window is "too small", not "invisible"; a cloaked one is cloaked).
 public static string Dump(int[] pids) {
   var sb = new StringBuilder();
   EnumWindows((h,p) => {
     uint pid; GetWindowThreadProcessId(h, out pid);
     bool mine=false; foreach(var q in pids){ if(q==(int)pid){mine=true;break;} }
     if (mine) {
       int cloaked=0; DwmGetWindowAttribute(h,14,out cloaked,4);
       RECT r; GetWindowRect(h,out r);
       sb.Append("  hwnd=0x"+h.ToInt64().ToString("X")+" visible="+IsWindowVisible(h)+" cloaked="+cloaked
         +" size="+(r.R-r.L)+"x"+(r.B-r.T)+" at="+r.L+","+r.T+"\n");
     } return true; }, IntPtr.Zero);
   return sb.ToString(); }
}
"@
# GetWindowRect answers in THIS process's DPI space. Unpinned, PowerShell is DPI-unaware and a
# correct 288x216 physical window on a 225% panel reads 128x96 - failing the size conjunct on
# a perfectly visible app. Pin PER_MONITOR_AWARE_V2 (-4) so sizes are physical px (CLAUDE.md DPI rule).
[void][VZ]::SetProcessDpiAwarenessContext([IntPtr]::new(-4))
if ($NoRuntime) { $env:XR_RUNTIME_JSON = "C:\displayxr-no-runtime-visibility-check.json" }
$name = [IO.Path]::GetFileNameWithoutExtension($Exe)
Start-Process -FilePath $Exe | Out-Null
$deadline = (Get-Date).AddSeconds($Seconds); $shown = ""
while ((Get-Date) -lt $deadline) {
  Start-Sleep -Milliseconds 800
  $pids = @(Get-Process -Name $name -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id)
  if ($pids.Count -gt 0) { $shown = [VZ]::Shown($pids); if ($shown -ne "") { break } }
}
$guardOk = $true
if ($GuardLog -ne "") {
  $guardOk = (Select-String -Path $GuardLog -Pattern "no OpenXR runtime resolvable" -Quiet) -eq $true
}
$dump = ""
if ($shown -eq "") { $pids = @(Get-Process -Name $name -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id); if ($pids.Count -gt 0) { $dump = [VZ]::Dump($pids) } }
Get-Process -Name $name -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
if ($shown -ne "" -and $guardOk) { Write-Output "PASS visible: $shown"; exit 0 }
if ($shown -eq "" -and $dump -ne "") { Write-Output "windows seen (need visible, cloaked=0, >200x200, on-screen):"; Write-Output $dump }
if ($shown -eq "") { Write-Output "FAIL INVISIBLE (no visible/uncloaked/on-screen window) - the #295 regression"; exit 1 }
Write-Output "FAIL guard log line absent - the app's own guard may be masking a broken plugin guard (#295)"; exit 1
