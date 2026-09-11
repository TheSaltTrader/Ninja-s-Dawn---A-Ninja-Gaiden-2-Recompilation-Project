# Re-encode an Xbox 360 .wmv to VC-1 Advanced Profile using the Expression
# Encoder 4 SDK. ffmpeg cannot do this: it has no VC-1 encoder, and its ASF
# container is rejected outright by the title's XMCORE demuxer. Only the EE4 /
# Windows Media Format SDK toolchain writes an ASF the XDK path accepts.
#
# MUST run under 32-bit PowerShell - the SDK is 32-bit and 64-bit throws
# BadImageFormatException:
#   C:\Windows\SysWOW64\WindowsPowerShell\v1.0\powershell.exe -File tools\encode_vc1.ps1 ...

param(
    [Parameter(Mandatory=$true)][string]$Source,
    [Parameter(Mandatory=$true)][string]$OutDir,
    [int]$Width    = 320,
    [int]$Height   = 576,
    [double]$Fps   = 60,
    [int]$Kbps     = 4000,
    [int]$Channels = 2,
    [switch]$AllIntra
)

$ErrorActionPreference = "Stop"
$enc = "C:\Program Files (x86)\Microsoft Expression\Encoder 4"
$env:Path = "$enc;$env:Path"
Add-Type -Path "$enc\SDK\Microsoft.Expression.Encoder.dll"
Add-Type -AssemblyName System.Drawing

Write-Output "source : $Source"
$job  = New-Object Microsoft.Expression.Encoder.Job
$item = New-Object Microsoft.Expression.Encoder.MediaItem($Source)

# Force exact output dimensions. By default EE4 preserves the source aspect
# ratio, which turned a requested 320x576 into 316x576 - the panels of this
# intro are composited side by side, so an off-by-4 width misaligns them.
$item.VideoResizeMode = [Microsoft.Expression.Encoder.VideoResizeMode]::Stretch

# The Xbox preset gets the container and muxing right; the video profile it
# selects is Main, which cannot carry 60fps, so it is replaced below.
$item.ApplyPreset([Microsoft.Expression.Encoder.Presets]::VC1Xbox360HD1080p)

$of  = $item.OutputFormat
$adv = New-Object Microsoft.Expression.Encoder.Profiles.AdvancedVC1VideoProfile
# AutoFit is what silently preserved the source aspect ratio and turned a
# requested 320x576 into 316x576. MediaItem.VideoResizeMode does NOT override
# it; this does. Set it before Size.
$adv.AutoFit   = $false
$adv.Size      = New-Object System.Drawing.Size($Width, $Height)
$adv.FrameRate = $Fps
$adv.Bitrate   = New-Object Microsoft.Expression.Encoder.Profiles.ConstantBitrate($Kbps)
# The stock NG2 intro videos are ALL-INTRA - every frame is a keyframe. That is
# not an encoder quirk, it is load-bearing: the title's GPU decoder has separate
# shader variants for Intra, Prog(ressive) and IntFrm(interlaced), and an
# all-intra stream never takes the predicted path, so it never does motion
# compensation against reference surfaces in EDRAM. Re-encoding with a normal
# I/P/B GOP forced that path and made the corruption worse.
if ($AllIntra) {
    $adv.BFrameCount      = 0
    $adv.AdaptiveGop      = $false
    $adv.ClosedGop        = $true
    $adv.KeyFrameDistance = [TimeSpan]::FromMilliseconds(1000.0 / $Fps)
}

$of.VideoProfile = $adv
$of.AudioProfile.Channels = $Channels

$job.MediaItems.Add($item)
$job.OutputDirectory = $OutDir
$job.CreateSubfolder = $false
$job.Encode()

Write-Output "encoded -> $OutDir"
