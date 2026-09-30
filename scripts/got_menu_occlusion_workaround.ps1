# Partial GoT menu workaround: restore geometry rejected by empty CPU depth maps.
# Black/white shading corruption remains unresolved. Applies only to the verified
# instruction sequence in the current CUSA11456 build; does not modify game files.
param(
    [ValidateRange(0, 1800)][int]$MaxSeconds = 0,
    [switch]$CaptureScreenshots,
    [switch]$ValidateOnly
)

& (Join-Path $PSScriptRoot 'got_diagnostic_launch.ps1') -Preset menu `
    -ExtraFlag 'SHADPS4_DIAG_GOT_SKIP_CPU_OCCLUSION=1' `
    -MaxSeconds $MaxSeconds -CaptureScreenshots:$CaptureScreenshots `
    -ValidateOnly:$ValidateOnly
