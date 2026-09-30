# Launch CUSA11456 with a recorded, process-scoped diagnostic configuration.
# Examples:
#   .\scripts\got_diagnostic_launch.ps1 -ValidateOnly
#   .\scripts\got_diagnostic_launch.ps1 -DeviceFault -CaptureScreenshots
#   .\scripts\got_diagnostic_launch.ps1 -Preset clean -LayerMode no-implicit -MaxSeconds 600
#   .\scripts\got_diagnostic_launch.ps1 -ExtraFlag SHADPS4_DIAG_GOT_COLOR0_READBACK=1
#   .\scripts\got_diagnostic_launch.ps1 -ExtraFlag SHADPS4_DIAG_GOT_FP16_CHAIN_READBACK=1,SHADPS4_DIAG_GOT_HEAD_VOLUME_CAPTURE=1
param(
    [ValidateSet('menu', 'clean')][string]$Preset = 'menu',
    [ValidateSet('normal', 'no-implicit')][string]$LayerMode = 'normal',
    [switch]$DeviceFault,
    [switch]$CaptureScreenshots,
    [switch]$ValidateOnly,
    [string[]]$ExtraFlag = @(),
    [ValidateRange(0, 1800)][int]$MaxSeconds = 0,
    [string]$Executable,
    [string]$Game = 'G:\ps5games\PS4\CUSA11456\eboot.bin',
    [string]$OutputRoot
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $Executable) { $Executable = Join-Path $repo 'Build\x64-Clang-Release\shadps4.exe' }
if (-not $OutputRoot) { $OutputRoot = Join-Path $repo 'Build\got-runs' }
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$Game = (Resolve-Path -LiteralPath $Game).Path
if (Get-Process -Name shadps4 -ErrorAction SilentlyContinue) {
    throw 'A shadPS4 process is already running. Close it before a comparable diagnostic launch.'
}

$flags = @{}
if ($Preset -eq 'menu') {
    $flags['SHADPS4_DIAG_CAP_UNRECOGNIZED_FS_LOOPS'] = '1'
}
if ($DeviceFault) { $flags['SHADPS4_DIAG_DEVICE_FAULT'] = '1' }
if ($CaptureScreenshots) { $flags['SHADPS4_DIAG_AUTO_SCREENSHOT'] = '1' }
if ($LayerMode -eq 'no-implicit') { $flags['VK_LOADER_LAYERS_DISABLE'] = '~implicit~' }
foreach ($entry in @($ExtraFlag | ForEach-Object { $_ -split ',' })) {
    if ($entry -notmatch '^(SHADPS4_DIAG_[A-Z0-9_]+)=(.*)$') {
        throw "Invalid diagnostic flag '$entry'; expected SHADPS4_DIAG_NAME=value"
    }
    $flags[$Matches[1]] = $Matches[2]
}

if ($ValidateOnly) {
    [pscustomobject]@{ Executable = $Executable; Game = $Game; Preset = $Preset;
        LayerMode = $LayerMode; Flags = $flags; MaxSeconds = $MaxSeconds } |
        ConvertTo-Json -Depth 4
    return
}

$runId = (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssfffZ') + "-$PID"
$runDir = Join-Path $OutputRoot $runId
New-Item -ItemType Directory -Path $runDir -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path (Split-Path -Parent $Executable) 'Build\got-ir-audit') -Force | Out-Null
$started = (Get-Date).ToUniversalTime()
$userDir = Join-Path $env:APPDATA 'shadPS4'
$screenshotDir = Join-Path $userDir 'screenshots'
$logDir = Join-Path $userDir 'log'
$launchExecutableHash = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash
$launchGitHead = git -C $repo rev-parse HEAD
$launchGitStatus = @(git -C $repo status --short)
$configPath = Join-Path $userDir 'config.json'
$launchConfigHash = if (Test-Path -LiteralPath $configPath) {
    (Get-FileHash -LiteralPath $configPath -Algorithm SHA256).Hash
} else { $null }
$titleId = Split-Path -Leaf (Split-Path -Parent $Game)
$launchProfileHash = $null
if ($titleId -match '^CUSA[0-9]{5}$') {
    $profilePath = Join-Path $userDir "custom_configs/$titleId.json"
    if (Test-Path -LiteralPath $profilePath) {
        Copy-Item -LiteralPath $profilePath -Destination (Join-Path $runDir 'per-game-config.json')
        $launchProfileHash = (Get-FileHash -LiteralPath (Join-Path $runDir 'per-game-config.json') -Algorithm SHA256).Hash
    }
}
$priorFlags = @{}
Get-ChildItem Env: | Where-Object { $_.Name -like 'SHADPS4_DIAG_*' -or
    $_.Name -eq 'VK_LOADER_LAYERS_DISABLE' } | ForEach-Object {
    $priorFlags[$_.Name] = $_.Value
    [Environment]::SetEnvironmentVariable($_.Name, $null, 'Process')
}
foreach ($key in $flags.Keys) {
    [Environment]::SetEnvironmentVariable($key, $flags[$key], 'Process')
}

$status = 'launch-failed'
$exitCode = $null
$stoppedByLimit = $false
try {
    $process = Start-Process -FilePath $Executable -ArgumentList ('"' + $Game + '"') `
        -WorkingDirectory (Split-Path -Parent $Executable) -PassThru `
        -RedirectStandardOutput (Join-Path $runDir 'stdout.log') `
        -RedirectStandardError (Join-Path $runDir 'stderr.log')
    # Retain the native handle before exit; a late handle lookup can lose ExitCode.
    $null = $process.Handle
    $status = 'running'
    if ($MaxSeconds -eq 0) {
        $process.WaitForExit()
    } elseif (-not $process.WaitForExit($MaxSeconds * 1000)) {
        $stoppedByLimit = $true
        $null = $process.CloseMainWindow()
        if (-not $process.WaitForExit(20000)) {
            Stop-Process -Id $process.Id -ErrorAction Stop
            $process.WaitForExit()
        }
    }
    $exitCode = $process.ExitCode
    $status = if ($stoppedByLimit) { 'time-limit' } else { 'exited' }
} finally {
    foreach ($key in $flags.Keys) {
        [Environment]::SetEnvironmentVariable($key, $null, 'Process')
    }
    foreach ($key in $priorFlags.Keys) {
        [Environment]::SetEnvironmentVariable($key, $priorFlags[$key], 'Process')
    }

    $copiedScreenshots = @()
    if (Test-Path -LiteralPath $screenshotDir) {
        Get-ChildItem -LiteralPath $screenshotDir -Filter 'CUSA11456*.png' -File |
            Where-Object { $_.LastWriteTimeUtc -ge $started } | ForEach-Object {
                Copy-Item -LiteralPath $_.FullName -Destination $runDir
                $copiedScreenshots += $_.Name
            }
    }
    $copiedLogs = @()
    if (Test-Path -LiteralPath $logDir) {
        Get-ChildItem -LiteralPath $logDir -File |
            Where-Object { $_.LastWriteTimeUtc -ge $started } | ForEach-Object {
                Copy-Item -LiteralPath $_.FullName -Destination $runDir
                $copiedLogs += $_.Name
            }
    }
    $copiedImages = @()
    $probeDirs = @((Join-Path $repo 'Build\got-ir-audit'),
        (Join-Path (Split-Path -Parent $Executable) 'Build\got-ir-audit'))
    foreach ($probeDir in ($probeDirs | Select-Object -Unique)) {
        if (Test-Path -LiteralPath $probeDir) {
            Get-ChildItem -LiteralPath $probeDir -File |
                Where-Object { $_.LastWriteTimeUtc -ge $started -and
                    ($_.Extension -in '.ppm', '.pgm' -or
                     ($_.Name -like 'fp16-head-*' -and $_.Extension -in '.bin', '.txt')) } |
                ForEach-Object {
                    Copy-Item -LiteralPath $_.FullName -Destination $runDir
                    $copiedImages += $_.Name
                }
        }
    }
    $ffmpeg = Get-Command ffmpeg -ErrorAction SilentlyContinue
    if ($ffmpeg) {
        Get-ChildItem -LiteralPath $runDir -Filter 'fp16-head-*.pgm' -File |
            ForEach-Object {
                $png = Join-Path $runDir ($_.BaseName + '.png')
                & $ffmpeg.Source -loglevel error -i $_.FullName -frames:v 1 $png
                if ($LASTEXITCODE -eq 0) { $copiedImages += (Split-Path -Leaf $png) }
            }
    }
    $gameLog = Join-Path $runDir 'shad_log.txt'
    $observedFailure = 'none-recorded'
    $failureLogs = @($gameLog, (Join-Path $runDir 'stdout.log'),
        (Join-Path $runDir 'stderr.log')) | Where-Object { Test-Path -LiteralPath $_ }
    if ($failureLogs.Count -gt 0) {
        if (Select-String -LiteralPath $failureLogs -Pattern 'Device lost during' -Quiet) {
            $observedFailure = 'vulkan-device-lost'
        } elseif (Select-String -LiteralPath $failureLogs -Pattern 'Unhandled exception' -Quiet) {
            # The exception handler also reports faults in emulator host code.
            # Attribute the origin from the captured instruction/context later.
            $observedFailure = 'unhandled-exception'
        } elseif (Select-String -LiteralPath $failureLogs -Pattern 'Assertion Failed!' -Quiet) {
            $observedFailure = 'assertion-failed'
        } elseif (Select-String -LiteralPath $failureLogs -Pattern 'Unreachable code!' -Quiet) {
            $observedFailure = 'unreachable-code'
        } elseif ($null -ne $exitCode -and $exitCode -ne 0 -and -not $stoppedByLimit) {
            $observedFailure = 'nonzero-exit-unclassified'
        }
    }
    $displayAdapters = @(Get-CimInstance Win32_VideoController |
        Select-Object Name, DriverVersion, PNPDeviceID)
    $metadata = [ordered]@{
        runId = $runId
        startedUtc = $started.ToString('o')
        endedUtc = (Get-Date).ToUniversalTime().ToString('o')
        status = $status
        observedFailure = $observedFailure
        exitCode = $exitCode
        executable = $Executable
        executableSha256 = $launchExecutableHash
        game = $Game
        gitHead = $launchGitHead
        gitStatus = $launchGitStatus
        configSha256 = $launchConfigHash
        perGameConfigSha256 = $launchProfileHash
        displayAdapters = $displayAdapters
        preset = $Preset
        layerMode = $LayerMode
        flags = $flags
        maxSeconds = $MaxSeconds
        screenshots = $copiedScreenshots
        probeImages = $copiedImages
        logs = $copiedLogs
    }
    $metadata | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $runDir 'run.json')
    Write-Output "Diagnostic run: $runDir ($status, exit code $exitCode)"
}
