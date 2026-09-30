# Best verified partial menu configuration for the current CUSA11456 checkout.
# Restores GPU-produced CPU depth maps and synchronizes the exposure compute shader.
# Particle/material defects remain; this is not a complete rendering fix.
param(
    [ValidateSet('menu', 'clean')][string]$Preset = 'menu',
    [ValidateRange(0, 1800)][int]$MaxSeconds = 0,
    [switch]$CaptureScreenshots,
    [switch]$ExperimentalSkyNoise,
    [switch]$ExperimentalMaterials,
    [string[]]$ExtraFlag = @(),
    [switch]$ValidateOnly
)

$ErrorActionPreference = 'Stop'
$launcher = Join-Path $PSScriptRoot 'got_diagnostic_launch.ps1'
$flags = @('SHADPS4_DIAG_GOT_FORCE_DC80_LDS_BARRIERS=1')
if ($ExperimentalSkyNoise) {
    $flags += 'SHADPS4_DIAG_GOT_DYNAMIC_SKY_NOISE_IMAGES=1'
}
$flags += $ExtraFlag
if ($ExperimentalMaterials) {
    $flags += 'SHADPS4_DIAG_GOT_DYNAMIC_IMAGE_ARRAY=1'
    $flags += 'SHADPS4_DIAG_GOT_DYNAMIC_SCENE_IMAGES=1'
    $flags += 'SHADPS4_DIAG_GOT_DYNAMIC_SECONDARY_IMAGE=1'
}

if ($ValidateOnly) {
    Write-Output 'Temporary CUSA11456 settings: Precise readbacks, image readbacks enabled; restored on exit.'
    & $launcher -Preset $Preset -ExtraFlag $flags -MaxSeconds $MaxSeconds `
        -CaptureScreenshots:$CaptureScreenshots -ValidateOnly
    return
}
if (Get-Process -Name shadps4 -ErrorAction SilentlyContinue) {
    throw 'Close the running emulator before starting this configuration.'
}

$profileDir = Join-Path $env:APPDATA 'shadPS4/custom_configs'
$profilePath = Join-Path $profileDir 'CUSA11456.json'
$existed = Test-Path -LiteralPath $profilePath
$original = if ($existed) { [IO.File]::ReadAllBytes($profilePath) } else { $null }
$profile = if ($existed) {
    [IO.File]::ReadAllText($profilePath) | ConvertFrom-Json
} else { [pscustomobject]@{} }
if (-not $profile.PSObject.Properties['GPU']) {
    $profile | Add-Member -NotePropertyName GPU -NotePropertyValue ([pscustomobject]@{})
}
$profile.GPU | Add-Member -NotePropertyName readbacks_mode -NotePropertyValue 2 -Force
$profile.GPU | Add-Member -NotePropertyName readback_linear_images_enabled -NotePropertyValue $true -Force
New-Item -ItemType Directory -Path $profileDir -Force | Out-Null
try {
    [IO.File]::WriteAllText($profilePath, ($profile | ConvertTo-Json -Depth 32))
    & $launcher -Preset $Preset -ExtraFlag $flags -MaxSeconds $MaxSeconds `
        -CaptureScreenshots:$CaptureScreenshots
} finally {
    if ($existed) {
        [IO.File]::WriteAllBytes($profilePath, $original)
    } elseif (Test-Path -LiteralPath $profilePath) {
        Remove-Item -LiteralPath $profilePath
    }
}
