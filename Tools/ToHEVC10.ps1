# =============================================================================
#  ToHEVC10.ps1 - Convert a video (ProRes 422 HQ .mov / EXR seq / any input)
#                 into a 10-bit H.265 (HEVC Main 10) MP4.
#
#  Why: UE 5.8's built-in MP4 output is H.264 8-bit only and defaults to
#       8 Mbps, which is far too low for 4K. This produces a true 10-bit
#       HEVC file with a sane bitrate.
#
#  Usage:
#     .\ToHEVC10.ps1 -Source "<你的视频.mov>"
#     .\ToHEVC10.ps1 -Source "<你的视频.mov>" -Cq 16 -Encoder x265
#     .\ToHEVC10.ps1 -Source "frames\shot_%04d.exr" -Fps 60
#
#  Parameters:
#     -Source   Source file or image-sequence pattern. Required.
#     -Output   Destination .mp4. Defaults to <Source>.10bit.mp4
#     -Cq       Quality, lower = better. 16=very high  19=high  22=medium
#     -Fps      Only used for image sequences (default 60).
#     -Encoder  nvenc (GPU, fast) | x265 (CPU, smaller/better). Default nvenc.
#
#  Note: the parameter is named -Source, not -Input, because $Input is a
#        PowerShell automatic variable and would be shadowed.
# =============================================================================
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Source,
    [string]$Output,
    [ValidateRange(0, 51)][int]$Cq = 19,
    [int]$Fps = 60,
    [ValidateSet('nvenc', 'x265')][string]$Encoder = 'nvenc'
)

$ErrorActionPreference = 'Stop'

# --- locate ffmpeg -----------------------------------------------------------
$ffmpeg = (Get-Command ffmpeg -ErrorAction SilentlyContinue).Source
if (-not $ffmpeg) {
    # Not on PATH - try the locations the common package managers install into.
    $candidates = @(
        "$env:USERPROFILE\scoop\shims\ffmpeg.exe"
        "$env:LOCALAPPDATA\Microsoft\WinGet\Links\ffmpeg.exe"
        "$env:ProgramData\chocolatey\bin\ffmpeg.exe"
        "$env:SystemDrive\ffmpeg\bin\ffmpeg.exe"
    )
    $ffmpeg = $candidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
}
if (-not $ffmpeg -or -not (Test-Path $ffmpeg)) {
    throw "ffmpeg not found. Install it and put it on PATH, then re-run."
}

# --- resolve input -----------------------------------------------------------
if ($Source -match '%0\d+d') {
    # image sequence pattern
    $probe = $Source -replace '%0\d+d', '0000'
    if (-not (Test-Path $probe)) { throw "Image sequence not found (probed: $probe)" }
    $inputs = @('-framerate', "$Fps", '-i', $Source)
    if (-not $Output) { $Output = (Split-Path $Source -Parent) + '\' + 'sequence.10bit.mp4' }
}
else {
    if (-not (Test-Path $Source)) { throw "Input not found: $Source" }
    $inputs = @('-i', $Source)
    if (-not $Output) { $Output = [IO.Path]::ChangeExtension($Source, $null).TrimEnd('.') + '.10bit.mp4' }
}

# --- build encoder args ------------------------------------------------------
if ($Encoder -eq 'nvenc') {
    # NVIDIA hardware HEVC Main 10. ~7.6 fps @ 4K on an RTX 4070 Ti SUPER.
    $encArgs = @(
        '-c:v', 'hevc_nvenc',
        '-profile:v', 'main10',
        '-pix_fmt', 'p010le',
        '-rc', 'vbr', '-cq', "$Cq", '-b:v', '0',
        '-preset', 'p7', '-tune', 'hq',
        '-tag:v', 'hvc1'          # required for QuickTime / most NLEs
    )
}
else {
    # libx265 software. Slower, but better quality per bitrate.
    $x265 = "aq-mode=3:psy-rd=2.0:psy-rdoq=1.0:sao=1"
    $encArgs = @(
        '-c:v', 'libx265',
        '-crf', "$Cq",
        '-pix_fmt', 'yuv420p10le',
        '-preset', 'slow',
        '-x265-params', $x265,
        '-tag:v', 'hvc1'
    )
}

$common = @(
    '-hide_banner', '-y', '-loglevel', 'error', '-stats',
    '-c:a', 'aac', '-b:a', '320k',
    '-movflags', '+faststart'
)

# --- run ---------------------------------------------------------------------
Write-Host "ffmpeg   : $ffmpeg"      -ForegroundColor DarkGray
Write-Host "encoder  : $Encoder (Cq/Crf $Cq)" -ForegroundColor Cyan
Write-Host "output   : $Output"      -ForegroundColor Cyan
Write-Host "encoding..."             -ForegroundColor DarkGray

$sw = [Diagnostics.Stopwatch]::StartNew()
& $ffmpeg @inputs @encArgs @common "$Output"
$code = $LASTEXITCODE
$sw.Stop()

if ($code -ne 0 -or -not (Test-Path $Output)) {
    throw "ffmpeg failed with exit code $code"
}

# --- verify ------------------------------------------------------------------
Write-Host ""
Write-Host "done in $([math]::Round($sw.Elapsed.TotalSeconds,1))s" -ForegroundColor Green

# ffmpeg prints its stream info to stderr; relax EAP so PowerShell doesn't
# turn that into a terminating NativeCommandError.
$prevEAP = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$probe = (& $ffmpeg -hide_banner -i "$Output" 2>&1 | Out-String)
$ErrorActionPreference = $prevEAP

$probe -split "`r?`n" |
    Where-Object { $_ -match 'Duration|Stream #0' } |
    ForEach-Object { Write-Host "  $($_.Trim())" -ForegroundColor DarkCyan }

$sizeMB = [math]::Round((Get-Item $Output).Length / 1MB, 1)
Write-Host "  size: $sizeMB MB" -ForegroundColor DarkCyan
Write-Host ""
if ($probe -match 'Main 10' -and $probe -match '10le|p010') {
    Write-Host "OK: real 10-bit HEVC." -ForegroundColor Green
} else {
    Write-Host "WARNING: output does not look like 10-bit HEVC - check the lines above." -ForegroundColor Yellow
}
