#requires -version 5.1
<#
.SYNOPSIS
  Captures the Assassin's Creed window (DPI-aware) to a PNG for visual feedback.

.PARAMETER Out
  Output PNG path. Defaults to AC1-RTX\tests\screens\<timestamp>.png.

.PARAMETER Width
  Width of the saved image (aspect preserved). 0 keeps full resolution.
#>
[CmdletBinding()]
param(
    [string]$Out = '',
    [int]$Width = 1600
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing
if (-not ('AcWindowCapture' -as [type])) {
    Add-Type @'
using System; using System.Runtime.InteropServices;
public static class AcWindowCapture {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  public struct RECT { public int L, T, R, B; }
  public static IntPtr Find(uint pid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => {
      uint p; GetWindowThreadProcessId(h, out p);
      RECT r;
      if (p == pid && IsWindowVisible(h) && GetWindowRect(h, out r) && r.R - r.L > 300) { found = h; }
      return true;
    }, IntPtr.Zero);
    return found;
  }
}
'@
}

$game = Get-Process AssassinsCreed_Dx9 -ErrorAction Stop | Select-Object -First 1
[AcWindowCapture]::SetProcessDPIAware() | Out-Null
$hwnd = [AcWindowCapture]::Find([uint32]$game.Id)
if ($hwnd -eq [IntPtr]::Zero) { throw 'Game window not found.' }
$rect = New-Object AcWindowCapture+RECT
[AcWindowCapture]::GetWindowRect($hwnd, [ref]$rect) | Out-Null

$full = New-Object Drawing.Bitmap ($rect.R - $rect.L), ($rect.B - $rect.T)
$graphics = [Drawing.Graphics]::FromImage($full)
$graphics.CopyFromScreen($rect.L, $rect.T, 0, 0, $full.Size)
$graphics.Dispose()

if (-not $Out) {
    $dir = Join-Path $PSScriptRoot '..\tests\screens'
    New-Item -ItemType Directory -Force $dir | Out-Null
    $Out = Join-Path $dir ((Get-Date -Format 'yyyyMMdd_HHmmss') + '.png')
}
$image = $full
if ($Width -gt 0 -and $full.Width -gt $Width) {
    $image = New-Object Drawing.Bitmap $full, $Width, ([int]($full.Height * $Width / $full.Width))
}
$image.Save($Out)
(Resolve-Path $Out).Path
