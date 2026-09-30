# Launch CUSA11456 and drive the main menu into a "Continue" gameplay load.
#
# The guest only accepts the menu through the virtual pad. Automated key injection is unreliable
# against the SDL window, so this temporarily binds the Cross button to the left mouse button in
# the *user* input config and clicks the menu entry in window-client coordinates. The original
# input file is restored byte-for-byte in `finally`, even on crash or Ctrl-C.
#
# Examples:
#   .\scripts\got_continue_launch.ps1 -MaxSeconds 420
#   .\scripts\got_continue_launch.ps1 -MaxSeconds 600 -DenseScreenshots
param(
    [ValidateRange(0, 3600)][int]$MaxSeconds = 420,
    [switch]$DenseScreenshots,
    [switch]$ExperimentalSkyNoise,
    [switch]$ExtraConfirmClick,
    [int]$MenuWaitSeconds = 35,
    [int]$PreClickDelaySeconds = 4,
    [string[]]$ExtraFlag = @()
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$wrapper = Join-Path $PSScriptRoot 'got_menu_rendering.ps1'
$inputDir = Join-Path $env:APPDATA 'shadPS4\input_config'
$inputPath = Join-Path $inputDir 'default.ini'

Add-Type @'
using System;
using System.Runtime.InteropServices;
public class GotWin {
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X,Y; }
}
'@

# The guest renders a 1920x1080 framebuffer that the host scales into its client area. Menu entry
# positions below are authored in framebuffer pixels and converted per launch.
$ContinueInFramebuffer = @{ x = 200; y = 265 }

function Get-EmulatorWindow {
    $proc = Get-Process -Name shadps4 -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    return $proc
}

function Invoke-MenuClick([IntPtr]$handle, [int]$fbX, [int]$fbY, [string]$label) {
    $client = New-Object GotWin+RECT
    [void][GotWin]::GetClientRect($handle, [ref]$client)
    $origin = New-Object GotWin+POINT
    $origin.X = 0
    $origin.Y = 0
    [void][GotWin]::ClientToScreen($handle, [ref]$origin)
    $point = New-Object GotWin+POINT
    $point.X = [int][math]::Round($fbX * $client.R / 1920.0)
    $point.Y = [int][math]::Round($fbY * $client.B / 1080.0)
    $screenX = $origin.X + $point.X
    $screenY = $origin.Y + $point.Y
    [void][GotWin]::SetForegroundWindow($handle)
    Start-Sleep -Milliseconds 700
    [void][GotWin]::SetCursorPos($screenX, $screenY)
    Start-Sleep -Milliseconds 350
    [GotWin]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 120
    [GotWin]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)
    Write-Output ("clicked {0} at client({1},{2}) screen({3},{4}) clientSize={5}x{6}" -f `
            $label, $point.X, $point.Y, $screenX, $screenY, $client.R, $client.B)
}

if (Get-Process -Name shadps4 -ErrorAction SilentlyContinue) {
    throw 'Close the running emulator before starting a comparable Continue launch.'
}

$inputExisted = Test-Path -LiteralPath $inputPath
$inputOriginal = if ($inputExisted) { [IO.File]::ReadAllBytes($inputPath) } else { $null }
$flags = @('SHADPS4_DIAG_GOT_B0_DYNAMIC_IMAGES=1')
if ($DenseScreenshots) { $flags += 'SHADPS4_DIAG_AUTO_SCREENSHOT_PERIOD=30' }
$flags += $ExtraFlag

$job = $null
try {
    if ($inputExisted) {
        $text = [IO.File]::ReadAllText($inputPath)
        $text = ($text -replace '(?m)^\s*cross\s*=.*$', 'cross = leftbutton')
        $text = ($text -replace '(?m)^\s*circle\s*=.*$', 'circle = rightbutton')
        if ($text -notmatch '(?m)^\s*cross\s*=') { $text += "`n`ncross = leftbutton`ncircle = rightbutton`n" }
        [IO.File]::WriteAllText($inputPath, $text)
        Write-Output "temporary input mapping installed (cross=leftbutton, circle=rightbutton)"
    } else {
        throw "Expected an existing input config at $inputPath"
    }

    $before = @(Get-ChildItem (Join-Path $repo 'Build\got-runs') -Directory -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty Name)

    $innerScript = Join-Path ([IO.Path]::GetTempPath()) "got_continue_inner_$PID.ps1"
    $doneMarker = Join-Path ([IO.Path]::GetTempPath()) "got_continue_done_$PID.txt"
    $flagList = ($flags -join ',')
    $sky = if ($ExperimentalSkyNoise) { 'true' } else { 'false' }
    $lines = @(
        "`$ErrorActionPreference = 'Continue'",
        "Set-Location '$repo'",
        "`$wrapperArgs = @('-ExperimentalSkyNoise:$sky', '-MaxSeconds', '$MaxSeconds', " +
            "'-CaptureScreenshots', '-ExtraFlag', '$flagList')",
        'try {',
        "    & '.\scripts\got_menu_rendering.ps1' `@wrapperArgs",
        "} catch {",
        "    ('inner error: ' + `$_) | Out-File -LiteralPath '$doneMarker' -Append",
        '} finally {',
        "    'done' | Out-File -LiteralPath '$doneMarker' -Append",
        '}'
    )
    [IO.File]::WriteAllText($innerScript, ($lines -join "`r`n"))
    Remove-Item -LiteralPath $doneMarker -ErrorAction SilentlyContinue
    $host_proc = Start-Process powershell.exe -PassThru -WindowStyle Hidden `
        -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $innerScript)

    $deadline = (Get-Date).AddSeconds($MenuWaitSeconds + 240)
    $window = $null
    while ((Get-Date) -lt $deadline) {
        $window = Get-EmulatorWindow
        if ($window) { break }
        if (Test-Path -LiteralPath $doneMarker) {
            Write-Output 'wrapper exited before the window appeared:'
            Get-Content -LiteralPath $doneMarker
            $innerOutput = Get-Content -LiteralPath $innerScript -Raw
            Write-Output $innerOutput
            throw 'Emulator never launched.'
        }
        Start-Sleep -Milliseconds 500
    }
    if (-not $window) { throw 'Emulator window never appeared.' }
    Write-Output "emulator window pid=$($window.Id) handle=$($window.MainWindowHandle)"

    Start-Sleep -Seconds $MenuWaitSeconds
    $window = Get-EmulatorWindow
    if (-not $window) { throw 'Emulator exited before the menu could be driven.' }

    Invoke-MenuClick $window.MainWindowHandle $ContinueInFramebuffer.x $ContinueInFramebuffer.y 'CONTINUE'
    if ($PreClickDelaySeconds -gt 0) { Start-Sleep -Seconds $PreClickDelaySeconds }
    if ($ExtraConfirmClick) {
        $window = Get-EmulatorWindow
        if ($window) {
            Invoke-MenuClick $window.MainWindowHandle 1100 700 'confirm (lower right)'
        }
    }

    $lastState = ''
    while (-not (Test-Path -LiteralPath $doneMarker)) {
        $proc = Get-Process -Name shadps4 -ErrorAction SilentlyContinue
        $state = if ($proc) { 'running' } else { 'exited' }
        if ($state -ne $lastState) {
            Write-Output "[$(Get-Date -Format HH:mm:ss)] emulator $state"
            $lastState = $state
        }
        Start-Sleep -Seconds 3
    }
    $null = $host_proc.WaitForExit(30000)
    Write-Output 'wrapper finished:'
    Get-Content -LiteralPath $doneMarker -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $innerScript, $doneMarker -ErrorAction SilentlyContinue
} finally {
    if ($inputExisted) {
        [IO.File]::WriteAllBytes($inputPath, $inputOriginal)
        Write-Output "input config restored (sha256 $((Get-FileHash -LiteralPath $inputPath -Algorithm SHA256).Hash))"
    } elseif (Test-Path -LiteralPath $inputPath) {
        Remove-Item -LiteralPath $inputPath
    }
    if (Get-Process -Name shadps4 -ErrorAction SilentlyContinue) {
        Write-Output 'WARNING: an emulator process is still running'
    }
}
