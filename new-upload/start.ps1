param(
    [string]$BindHost = '127.0.0.1',
    [int]$Port = 8000,
    [string]$PythonExe = '',
    [string]$AllowedRoot = ''
)
$ErrorActionPreference = 'Stop'
if (-not $PythonExe) {
    $localVenv = Join-Path $PSScriptRoot '.venv\Scripts\python.exe'
    $bundledPython = Join-Path $env:USERPROFILE '.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
    if (Test-Path -LiteralPath $localVenv) { $PythonExe = $localVenv }
    elseif (Test-Path -LiteralPath $bundledPython) { $PythonExe = $bundledPython }
    else { $PythonExe = 'python' }
}
$serverArgs = @((Join-Path $PSScriptRoot 'upload_server.py'), '--host', $BindHost, '--port', "$Port")
if ($AllowedRoot) { $serverArgs += @('--allowed-root', $AllowedRoot) }
& $PythonExe @serverArgs
exit $LASTEXITCODE
