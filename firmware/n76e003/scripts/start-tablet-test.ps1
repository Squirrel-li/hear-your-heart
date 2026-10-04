[CmdletBinding()]
param(
 [string]$Device='95595018',
 [ValidateRange(1024,65535)][int]$HttpPort=8771,
 [string]$AdbPath='C:\itap-tools\android-sdk\platform-tools\adb.exe',
 [string]$PythonPath='python',
 [ValidateRange(0,86400)][int]$Seconds=0,
 [switch]$NoOpen
)
$ErrorActionPreference='Stop'
$server=$null;$temporary=$null;$forwarded=$false;$consoleInput=$false;$originalCtrlC=$false
function Invoke-Adb([string[]]$Arguments) {
 $output=& $script:AdbPath @Arguments 2>&1
 if ($LASTEXITCODE -ne 0) { throw "ADB failed: $output" }
 return ($output -join "`n")
}
try {
 if (-not (Test-Path -LiteralPath $AdbPath)) {
  $AdbPath=(Get-Command adb -ErrorAction Stop).Source
 }
 $PythonPath=(Get-Command $PythonPath -ErrorAction Stop).Source
 if ($Device -notmatch '^[a-zA-Z0-9_.:-]+$') { throw 'Invalid ADB device identifier.' }
 if ((Invoke-Adb @('-s',$Device,'get-state')).Trim() -ne 'device') { throw 'Tablet is not authorized/online in ADB.' }
 $mapping="tcp:$HttpPort"
 if ((Invoke-Adb @('-s',$Device,'reverse','--list')) -match "(?m)\s$([regex]::Escape($mapping))\s") { throw "An ADB reverse mapping for $mapping already exists; choose another -HttpPort." }
 $probe=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,$HttpPort)
 try { $probe.Start() } finally { $probe.Stop() }
 $temporary=Join-Path ([IO.Path]::GetTempPath()) ('itap-bm7701-'+[guid]::NewGuid().ToString('N'))
 $null=New-Item -ItemType Directory -Path $temporary
 # Serve only this page, never UART logs or other workspace files.
 Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'ble-test.html') -Destination $temporary
 $server=Start-Process -FilePath $PythonPath -ArgumentList @('-m','http.server',"$HttpPort",'--bind','127.0.0.1','--directory',('"'+$temporary+'"')) -WindowStyle Hidden -PassThru
 $ready=$false
 for ($i=0;$i -lt 30;$i++) {
  if ($server.HasExited) { throw 'Python HTTP server exited during startup.' }
  try { $response=Invoke-WebRequest -Uri "http://127.0.0.1:$HttpPort/ble-test.html" -UseBasicParsing -TimeoutSec 1; $ready=($response.StatusCode -eq 200) } catch { }
  if ($ready) { break }
  Start-Sleep -Milliseconds 100
 }
 if (-not $ready -or $server.HasExited) { throw 'Local HTTP server did not start.' }
 $null=Invoke-Adb @('-s',$Device,'reverse','--no-rebind',$mapping,$mapping)
 $forwarded=$true
 $url="http://localhost:$HttpPort/ble-test.html"
 if (-not $NoOpen) { $null=Invoke-Adb @('-s',$Device,'shell','am','start','-a','android.intent.action.VIEW','-d',$url,'-p','com.android.chrome') }
 Write-Host "Tablet page: $url"
 Write-Host 'Keep this process running. Press Q or Ctrl+C to stop and remove the USB reverse mapping.'
 # Read Ctrl+C as a key so PowerShell does not cancel native ADB cleanup in finally.
 $consoleInput=-not [Console]::IsInputRedirected
 if ($consoleInput) { $originalCtrlC=[Console]::TreatControlCAsInput;[Console]::TreatControlCAsInput=$true }
 $clock=[Diagnostics.Stopwatch]::StartNew()
 while ($Seconds -eq 0 -or $clock.Elapsed.TotalSeconds -lt $Seconds) {
  if ($consoleInput -and [Console]::KeyAvailable) {
   $key=[Console]::ReadKey($true)
   if ($key.Key -eq 'Q' -or ([int]$key.KeyChar -eq 3)) { break }
  }
  if ($server.HasExited) { throw 'HTTP server exited unexpectedly.' }
  Start-Sleep -Milliseconds 250
 }
} catch { Write-Error $_ -ErrorAction Continue;exit 1 }
finally {
 if ($consoleInput) { [Console]::TreatControlCAsInput=$originalCtrlC }
 if ($forwarded) {
  try { $null=Invoke-Adb @('-s',$Device,'reverse','--remove',"tcp:$HttpPort") } catch { Write-Warning "Could not remove ADB reverse mapping: $_" }
 }
 if ($null -ne $server -and -not $server.HasExited) { $server | Stop-Process }
 if ($temporary -and (Test-Path -LiteralPath $temporary)) {
  $resolved=[IO.Path]::GetFullPath($temporary)
  $tempRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
  if ($resolved.StartsWith($tempRoot,[StringComparison]::OrdinalIgnoreCase) -and [IO.Path]::GetFileName($resolved) -match '^itap-bm7701-[a-f0-9]{32}$') {
   Remove-Item -LiteralPath $resolved -Recurse -Force
  }
 }
}

