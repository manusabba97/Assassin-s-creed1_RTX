#requires -version 5.1
<#
.SYNOPSIS
  Deploys the Remix Plus bridge (x86 client + x64 server) and runtime into a target directory.

.DESCRIPTION
  Layout produced:
    <Target>\d3d9.dll               bridge client (x86)
    <Target>\NvRemixLauncher32.exe  bridge launcher (x86)
    <Target>\.trex\bridge.conf      bridge config with the Remix API exposed
    <Target>\.trex\NvRemixBridge.exe  bridge server (x64)
    <Target>\.trex\*                Remix Plus runtime (x64 d3d9.dll + dependencies)

.PARAMETER Target
  Directory that contains the 32-bit executable.

.PARAMETER RemixRoot
  dxvk-remix checkout with built _output\ and bridge\_output\.

.PARAMETER Windowed
  Sets client.forceWindowed = True in bridge.conf (game ignores its own Fullscreen=0 setting).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$Target,
    [string]$RemixRoot = 'C:\Users\manu\Desktop\MOD\dxvk-remix',
    [switch]$Windowed
)

$ErrorActionPreference = 'Stop'

$bridgeOut = Join-Path $RemixRoot 'bridge\_output'
$runtimeOut = Join-Path $RemixRoot '_output'
$trex = Join-Path $Target '.trex'

foreach ($required in @("$bridgeOut\d3d9.dll", "$bridgeOut\.trex\NvRemixBridge.exe", "$runtimeOut\d3d9.dll")) {
    if (-not (Test-Path $required)) { throw "Missing build artifact: $required" }
}

New-Item -ItemType Directory -Force $trex | Out-Null

Copy-Item "$bridgeOut\d3d9.dll", "$bridgeOut\NvRemixLauncher32.exe" $Target -Force
Copy-Item "$bridgeOut\.trex\NvRemixBridge.exe" $trex -Force
Get-ChildItem $runtimeOut -Force | Where-Object { $_.Extension -notin '.lib', '.exp' } |
    Copy-Item -Destination $trex -Recurse -Force

$conf = Get-Content (Join-Path $RemixRoot 'bridge\bridge.conf')
$conf = $conf -replace '^#\s*exposeRemixApi\s*=.*$', 'exposeRemixApi = True'
if ($Windowed) {
    $conf = $conf -replace '^#\s*client\.forceWindowed\s*=.*$', 'client.forceWindowed = True'
}
Set-Content -Path (Join-Path $trex 'bridge.conf') -Value $conf -Encoding ascii

Write-Host "Deployed Remix bridge + runtime to $Target"
