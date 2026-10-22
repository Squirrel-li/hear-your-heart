param([string]$Python = "$env:USERPROFILE/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe")
$ErrorActionPreference = 'Stop'
Write-Output 'Chrome / Edge: http://localhost:8772/ble_capture.html'
Write-Output 'Keep this window open; Ctrl+C stops the local server. No UART cable is needed.'
& $Python -m http.server 8772 --bind 127.0.0.1 --directory $PSScriptRoot
if ($LASTEXITCODE) { throw 'Server failed. Check port 8772 or pass -Python with your Python executable.' }
