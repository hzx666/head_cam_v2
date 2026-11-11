param([string]$Version = '', [string]$DistPath = '', [string]$OssutilPath = 'ossutil', [string]$OssConfig = '', [switch]$Upload)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (!$DistPath) { $DistPath = Join-Path $root 'dist' }
$binary = (Resolve-Path -LiteralPath (Join-Path $DistPath 'head-cam-service')).Path
$stream = [IO.File]::OpenRead($binary)
try {
    if ($stream.Length -lt 64) { throw 'Invalid release binary' }
    $null = $stream.Seek(-64, [IO.SeekOrigin]::End)
    $tail = New-Object byte[] 64
    $null = $stream.Read($tail, 0, 64)
} finally { $stream.Dispose() }
$tag = [Text.Encoding]::ASCII.GetString($tail)
if ($tag -notmatch '\nHEAD_CAM_RELEASE=([0-9]{8}\.[0-9]{3})\n$') { throw 'Missing embedded release version; rebuild first.' }
$builtVersion = $Matches[1]
if (!$Version) { $Version = $builtVersion }
if ($Version -ne $builtVersion) { throw 'Version does not match embedded release version.' }
$stage = Join-Path $root ('artifacts/oss-release/' + $Version + '-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage -Force | Out-Null
$copy = Join-Path $stage 'head-cam-service'
Copy-Item -LiteralPath $binary -Destination $copy
$sha = (Get-FileHash -LiteralPath $copy -Algorithm SHA256).Hash.ToLowerInvariant()
$target = "oss://ss-oss-data-platform/tools/device_monitor/head_cam_v2/$Version/"
Write-Output "Validated release: $copy"
Write-Output "Embedded version: $Version"
Write-Output "SHA256: $sha"
Write-Output "OSS destination: $target"
if (!$Upload) { Write-Output 'Preview only. Add -Upload to publish.'; exit 0 }
$cli = (Get-Command $OssutilPath -ErrorAction Stop).Source
$common = @('--connect-timeout=10', '--read-timeout=30', '--retry-times=2')
if ($OssConfig) { $common += @('-c', (Resolve-Path -LiteralPath $OssConfig).Path) }
$listing = & $cli @common ls $target -s 2>&1
if ($LASTEXITCODE -ne 0) { throw 'Cannot list OSS destination.' }
if (($listing | Out-String).Contains($target)) { throw 'Remote version already exists; use a new version.' }
# A single object is published atomically with integrity metadata, no sidecars.
$null = & $cli @common cp $copy ($target + 'head-cam-service') --meta "x-oss-meta-sha256:$sha" -f 2>&1
if ($LASTEXITCODE -ne 0) { throw 'Release publication failed.' }
Write-Output "Published: $target"
