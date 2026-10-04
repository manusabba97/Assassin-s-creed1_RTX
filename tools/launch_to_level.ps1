#requires -version 5.1
<#
.SYNOPSIS
  Starts AssassinsCreed_Dx9.exe and presses RETURN every 0.5 s (Seconds, then again for ExtraSeconds after PauseSeconds)
  to get from the menus and the Abstergo/Animus sequence into Masyaf.

.DESCRIPTION
  Relies on EaglePatch SkipIntroVideos=1 (no intro videos). Input is sent with livetools gamectl
  (SendInput + focus management), so it also works with the game's DirectInput handling.
  Prints the game PID at the end so livetools can attach to it.

.PARAMETER Seconds
  Total time to keep pressing RETURN.

.PARAMETER IntervalMs
  Delay between RETURN presses.
#>
[CmdletBinding()]
param(
    [int]$Seconds = 40,
    [int]$PauseSeconds = 10,
    [int]$ExtraSeconds = 0,
    [int]$IntervalMs = 500,
    [string]$GameDir = 'C:\Program Files\GOG Galaxy\Games\Assassins Creed',
    [string]$ToolsRoot = 'C:\Users\manu\Desktop\MOD\Vibe-Reverse-Engineering'
)

$ErrorActionPreference = 'Stop'

$exe = 'AssassinsCreed_Dx9.exe'
if (Get-Process ([IO.Path]::GetFileNameWithoutExtension($exe)) -ErrorAction SilentlyContinue) {
    throw "$exe is already running."
}

# DXVK HUD fps counter: set by the mod itself (AC1RTX.ini [Debug] FpsHud).
$game = Start-Process (Join-Path $GameDir $exe) -WorkingDirectory $GameDir -PassThru

$deadline = (Get-Date).AddSeconds(30)
while ($game.MainWindowHandle -eq 0) {
    if ($game.HasExited) { throw "$exe exited before creating its window (exit code $($game.ExitCode))." }
    if ((Get-Date) -gt $deadline) { throw "$exe did not create a window within 30 s." }
    Start-Sleep -Milliseconds 250
    $game.Refresh()
}

$python = Join-Path $ToolsRoot '.venv\Scripts\python.exe'
function Send-Return([int]$DurationSeconds) {
    $presses = [int]($DurationSeconds * 1000 / $IntervalMs)
    if ($presses -le 0) { return }
    $sequence = (@("RETURN WAIT:$IntervalMs") * $presses) -join ' '
    Push-Location $ToolsRoot
    try {
        & $python -m livetools gamectl --exe $exe keys $sequence
    } finally {
        Pop-Location
    }
}

Send-Return $Seconds
# Second burst: the Abstergo lab / Animus sequence can still be waiting for RETURN after the first one.
if ($ExtraSeconds -gt 0) {
    Start-Sleep -Seconds $PauseSeconds
    $game.Refresh()
    if ($game.HasExited) { throw "$exe exited during menu navigation (exit code $($game.ExitCode))." }
    Send-Return $ExtraSeconds
}

$game.Refresh()
if ($game.HasExited) { throw "$exe exited during menu navigation (exit code $($game.ExitCode))." }
Write-Host "In level. PID $($game.Id)"
