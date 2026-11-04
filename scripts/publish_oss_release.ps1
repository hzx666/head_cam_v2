param(
    [string]$Version = '',
    [string]$DistPath = '',
    [string]$OssutilPath = 'ossutil',
    [string]$OssConfig = '',
    [switch]$Upload
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (!$DistPath) { $DistPath = Join-Path $root 'dist' }
$dist = (Resolve-Path -LiteralPath $DistPath).Path
$builtVersion = (Get-Content -Raw -LiteralPath (Join-Path $dist 'SERVICE_VERSION')).Trim()
if (!$Version) { $Version = $builtVersion }
if ($Version -notmatch '^[0-9]{8}\.[0-9]{3}$' -or $Version -ne $builtVersion) {
    throw 'Version must match dist/SERVICE_VERSION; change SERVICE_VERSION and rebuild first.'
}
$files = @('head-cam-service', 'service_entry.sh', 'run-upload', 'run-monitor', 'boot_autostart.sh', 'manual_dhcp.sh', 'monitor.json', 'SERVICE_VERSION')
$manifest = @{}
foreach ($line in Get-Content -LiteralPath (Join-Path $dist 'SHA256SUMS')) {
    if ($line -notmatch '^([0-9a-f]{64})  ([a-zA-Z0-9_.-]+)$') { throw 'Invalid SHA256SUMS' }
    if ($manifest.ContainsKey($Matches[2])) { throw 'Duplicate manifest entry' }
    $manifest[$Matches[2]] = $Matches[1]
}
if ($manifest.Count -ne $files.Count) { throw 'Unexpected manifest file count' }
foreach ($name in $files) {
    $path = Join-Path $dist $name
    if (!(Test-Path -LiteralPath $path -PathType Leaf) -or !$manifest.ContainsKey($name)) { throw "Missing release file: $name" }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $manifest[$name]) { throw "Checksum mismatch: $name" }
}
# 生成独立发布副本，不改设备联调 JSON，也不发布任何 runtime/日志/任务。
$stage = Join-Path $root ('artifacts/oss-release/' + $Version + '-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage -Force | Out-Null
foreach ($name in $files) { Copy-Item -LiteralPath (Join-Path $dist $name) -Destination (Join-Path $stage $name) }
$encoding = New-Object Text.UTF8Encoding($false)
$config = Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $stage 'monitor.json') | ConvertFrom-Json
$config.device_id = ''
$config.enabled = $false
[IO.File]::WriteAllText((Join-Path $stage 'monitor.json'), ($config | ConvertTo-Json -Depth 20) + "`n", $encoding)
$lines = foreach ($name in $files) {
    (Get-FileHash -LiteralPath (Join-Path $stage $name) -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + $name
}
[IO.File]::WriteAllText((Join-Path $stage 'SHA256SUMS'), ($lines -join "`n") + "`n", [Text.Encoding]::ASCII)
$target = "oss://ss-oss-data-platform/tools/device_monitor/head_cam_v2/$Version/"
Write-Output "Validated release: $stage"
Write-Output "OSS destination: $target"
if (!$Upload) { Write-Output 'Preview only. Add -Upload to publish.'; exit 0 }
$command = Get-Command $OssutilPath -ErrorAction Stop
$cli = $command.Source
$common = @('--connect-timeout=10', '--read-timeout=30', '--retry-times=2')
if ($OssConfig) { $common += @('-c', (Resolve-Path -LiteralPath $OssConfig).Path) }
# 不透传 CLI 输出，避免意外暴露认证诊断。发布由单一操作者串行执行。
$listing = & $cli @common ls $target -s 2>&1
if ($LASTEXITCODE -ne 0) { throw 'Cannot list OSS destination; check publisher authentication and permissions.' }
if (($listing | Out-String).Contains($target)) { throw 'Remote version already contains objects. Use a new version; overwriting is forbidden.' }
foreach ($name in $files) {
    $null = & $cli @common cp (Join-Path $stage $name) ($target + $name) -f 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Upload failed: $name. Release marker was not published." }
}
# 最后上传清单作为发布完成标志；设备只发现带该标志的版本。
$null = & $cli @common cp (Join-Path $stage 'SHA256SUMS') ($target + 'SHA256SUMS') -f 2>&1
if ($LASTEXITCODE -ne 0) { throw 'Failed to publish completion manifest.' }
Write-Output "Published: $target"
