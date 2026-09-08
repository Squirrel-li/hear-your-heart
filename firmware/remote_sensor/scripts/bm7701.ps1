[CmdletBinding()]
param(
 [ValidateSet('Ports','Version','Status','Name','Advertise','Power','Send','Receive','Counter','Save','Shell','Web')][string]$Command='Shell',
 [string]$Port='COM11', [ValidateRange(1,4000000)][int]$Baud=115200,
 [string]$Value='', [ValidateSet('Hex','Text')][string]$Encoding='Hex',
 [ValidateRange(100,60000)][int]$TimeoutMs=3000,
 [ValidateRange(0,86400)][int]$Seconds=0,
 [ValidateRange(1,65536)][int]$Count=100,
 [ValidateRange(10,60000)][int]$IntervalMs=100,
 [ValidateRange(1024,65535)][int]$HttpPort=8771, [string]$LogPath
)
$ErrorActionPreference='Stop'
function ConvertTo-BmBytes([string]$Text,[string]$Format='Hex') {
 if ($Format -eq 'Text') { return ,([Text.Encoding]::UTF8.GetBytes($Text)) }
 $hex=$Text -replace '\s',''
 if ($hex.Length % 2 -ne 0 -or $hex -match '[^0-9a-fA-F]') { throw 'HEX needs complete byte pairs, e.g. 00 01 FF.' }
 $bytes=[byte[]]::new($hex.Length/2)
 for ($i=0;$i -lt $bytes.Length;$i++) { $bytes[$i]=[Convert]::ToByte($hex.Substring($i*2,2),16) }
 return ,$bytes
}
function Format-BmHex([byte[]]$Bytes) { return [BitConverter]::ToString($Bytes).Replace('-',' ') }
function New-BmFrame([int]$Type,[int]$Flag=0,[byte[]]$Data=@()) {
 if ($Type -lt 0 -or $Type -gt 65535 -or $Flag -lt 0 -or $Flag -gt 255 -or $Data.Length -gt 252) { throw 'Invalid BCI frame fields.' }
 return ,([byte[]](@(0x77,($Data.Length+3),$Flag,($Type -band 255),($Type -shr 8))+$Data))
}
function New-BmParser {
 return @{Buffer=[Collections.Generic.List[byte]]::new();Frames=[Collections.Generic.Queue[object]]::new()}
}
function Add-BmBytes($Parser,[byte[]]$Bytes) {
 $Parser.Buffer.AddRange($Bytes)
 while ($Parser.Buffer.Count -ge 2) {
  if ($Parser.Buffer[0] -ne 0x78 -or $Parser.Buffer[1] -lt 3) { $Parser.Buffer.RemoveAt(0);continue }
  $size=2+[int]$Parser.Buffer[1]
  if ($Parser.Buffer.Count -lt $size) { break }
  $raw=$Parser.Buffer.GetRange(0,$size).ToArray()
  $Parser.Buffer.RemoveRange(0,$size)
  $data=[byte[]]::new($size-5)
  if ($data.Length) { [Array]::Copy($raw,5,$data,0,$data.Length) }
  $Parser.Frames.Enqueue([pscustomobject]@{Raw=$raw;Status=[int]$raw[2];Type=([int]$raw[3] -bor ([int]$raw[4] -shl 8));Data=$data})
 }
}
function Test-BmReply($Frame,[int]$Type,[int]$Flag) {
 # FFF2 is unsolicited data, never a command acknowledgement.
 return ($Frame.Type -ne 0xFFF2 -and $Frame.Type -eq $Type -and ($Frame.Status -band 0xF0) -eq ($Flag -band 0xF0))
}
function Assert-BmSuccess($Frame) {
 if (($Frame.Status -band 15) -ne 0) { throw ('BM7701 error: type=0x{0:X4}, status=0x{1:X2}' -f $Frame.Type,$Frame.Status) }
}
function Write-BmLog([string]$Message) {
 $line='{0} {1}' -f [DateTimeOffset]::Now.ToString('o'),$Message
 Write-Host $line
 if ($script:LogPath) { Add-Content -LiteralPath $script:LogPath -Value $line -Encoding UTF8 }
 if ($null -ne $script:WebLog) {
  $script:WebLogId++
  $script:WebLog.Add([pscustomobject]@{id=$script:WebLogId;text=$line})
  if ($script:WebLog.Count -gt 2000) { $script:WebLog.RemoveAt(0) }
 }
}
function Show-BmFrame($Frame) {
 Write-BmLog ('RX {0} | type=0x{1:X4} status=0x{2:X2}' -f (Format-BmHex $Frame.Raw),$Frame.Type,$Frame.Status)
 if ($Frame.Type -eq 0xFFF2) {
  Write-BmLog ('BLE WRITE {0} | UTF8={1}' -f (Format-BmHex $Frame.Data),([Text.Encoding]::UTF8.GetString($Frame.Data) -replace '[\x00-\x1F\x7F]','.'))
 } elseif ($Frame.Type -eq 0 -and $Frame.Data.Length -gt 0) {
  $script:LinkConnected=(($Frame.Data[0] -band 1) -ne 0)
  Write-BmLog ('LINK connected={0}; raw status={1}' -f $script:LinkConnected,(Format-BmHex $Frame.Data))
 }
}
function Read-BmFrames {
 if (-not $script:Serial.IsOpen) { throw 'Serial port disconnected.' }
 $available=$script:Serial.BytesToRead
 if ($available -gt 0) {
  $bytes=[byte[]]::new($available)
  $n=$script:Serial.Read($bytes,0,$bytes.Length)
  if ($n -ne $bytes.Length) { $bytes=[byte[]]$bytes[0..($n-1)] }
  Add-BmBytes $script:Parser $bytes
  $script:LastByteAt=$script:Clock.ElapsedMilliseconds
 }
 if ($script:Parser.Buffer.Count -gt 0 -and ($script:Clock.ElapsedMilliseconds-$script:LastByteAt) -gt $script:TimeoutMs) {
  Write-BmLog ('WARN incomplete frame expired: '+(Format-BmHex $script:Parser.Buffer.ToArray()))
  $script:Parser.Buffer.Clear()
 }
 while ($script:Parser.Frames.Count -gt 0) {
  $frame=$script:Parser.Frames.Dequeue()
  Show-BmFrame $frame
  $frame
 }
}
function Invoke-BmCommand([int]$Type,[int]$Flag=0,[byte[]]$Data=@()) {
 # ponytail: one outstanding command; close on timeout because BCI has no transaction ID.
 $null=@(Read-BmFrames)
 $raw=New-BmFrame $Type $Flag $Data
 Write-BmLog ('TX '+(Format-BmHex $raw))
 $script:Serial.Write($raw,0,$raw.Length)
 $deadline=$script:Clock.ElapsedMilliseconds+$script:TimeoutMs
 while ($script:Clock.ElapsedMilliseconds -lt $deadline) {
  $match=$null
  foreach ($frame in @(Read-BmFrames)) {
   if ($null -eq $match -and (Test-BmReply $frame $Type $Flag)) { $match=$frame }
  }
  if ($null -ne $match) { Assert-BmSuccess $match;return $match }
  Start-Sleep -Milliseconds 5
 }
 throw ('Timeout waiting for 0x{0:X4}; close session to avoid matching a late ACK to a new command.' -f $Type)
}
function Wait-BmData([int]$Milliseconds) {
 $deadline=$script:Clock.ElapsedMilliseconds+$Milliseconds
 do { $null=@(Read-BmFrames);Start-Sleep -Milliseconds 5 } while ($script:Clock.ElapsedMilliseconds -lt $deadline)
}
function Invoke-BmAction([string]$Action,[string]$Argument='',[string]$Format='Hex',[int]$ActionCount=0,[int]$ActionIntervalMs=0) {
 if ($ActionCount -le 0) { $ActionCount=$script:Count }
 if ($ActionIntervalMs -le 0) { $ActionIntervalMs=$script:IntervalMs }
 switch ($Action.ToLowerInvariant()) {
  'version' { $null=Invoke-BmCommand 0x20 }
  'status' { $null=Invoke-BmCommand 0 }
  'name' {
   $data=ConvertTo-BmBytes $Argument Text
   if ($data.Length -lt 1 -or $data.Length -gt 31 -or $Argument -match '[\x00-\x1F\x7F]') { throw 'Name must be 1..31 UTF-8 bytes without control characters.' }
   $null=Invoke-BmCommand 5 0 $data
  }
  'advertise' {
   if ($Argument -notin @('on','off')) { throw 'Advertise requires on or off.' }
   $null=Invoke-BmCommand 7 0 ([byte[]]@([byte]($Argument -eq 'on')))
  }
  'power' {
   if ($Argument -notmatch '^(?:[0-9]|1[0-5])$') { throw 'Power must be 0..15.' }
   $null=Invoke-BmCommand 11 0 ([byte[]]@([byte]$Argument))
  }
  'send' {
   $data=ConvertTo-BmBytes $Argument $Format
   # ponytail: conservative library limit (<128); no fragmentation until MTU is verified.
   if ($data.Length -lt 1 -or $data.Length -gt 127) { throw 'Send requires 1..127 bytes; start with <=20 bytes.' }
   $null=Invoke-BmCommand 0xFFF1 0x20 $data
   Write-BmLog 'UART ACK only; verify delivery in the BLE client notification log.'
  }
  'counter' {
   for ($i=0;$i -lt $ActionCount;$i++) {
    $data=[byte[]]@(0x42,0x4D,($i -band 255),($i -shr 8),0,1,2,3,4,5)
    $null=Invoke-BmCommand 0xFFF1 0x20 $data
    Wait-BmData $ActionIntervalMs
   }
   Write-BmLog ('Counter complete: {0} UART ACKs; compare BLE client results.' -f $ActionCount)
  }
  'save' { $null=Invoke-BmCommand 0x10 0x10 ([byte[]]@(0x10,0,0,0));Write-BmLog 'Flash save requested. No automatic reset.' }
  default { throw "Unknown action: $Action" }
 }
}

function ConvertFrom-BmWebAction($Payload) {
 if ($null -eq $Payload -or [string]::IsNullOrWhiteSpace([string]$Payload.action)) { throw 'API action is required.' }
 $action=([string]$Payload.action).ToLowerInvariant()
 if ($action -notin @('version','status','name','advertise','power','send','counter','save')) { throw "Unsupported API action: $action" }
 $format=if ($null -eq $Payload.encoding -or [string]::IsNullOrWhiteSpace([string]$Payload.encoding)) { 'Hex' } else { [string]$Payload.encoding }
 if ($format -notin @('Hex','Text')) { throw 'API encoding must be Hex or Text.' }
 $argument=if ($null -eq $Payload.value) { '' } else { [string]$Payload.value }
 $count=if ($null -eq $Payload.count -or [string]::IsNullOrWhiteSpace([string]$Payload.count)) { $script:Count } else { [int]$Payload.count }
 $interval=if ($null -eq $Payload.intervalMs -or [string]::IsNullOrWhiteSpace([string]$Payload.intervalMs)) { $script:IntervalMs } else { [int]$Payload.intervalMs }
 if ($count -lt 1 -or $count -gt 65536) { throw 'API count must be 1..65536.' }
 if ($interval -lt 10 -or $interval -gt 60000) { throw 'API intervalMs must be 10..60000.' }
 [pscustomobject]@{Action=$action;Argument=$argument;Format=$format;Count=$count;IntervalMs=$interval}
}

function Write-BmWebResponse([System.Net.HttpListenerContext]$Context,[int]$StatusCode,[string]$ContentType,[byte[]]$Bytes) {
 $Context.Response.StatusCode=$StatusCode
 $Context.Response.ContentType=$ContentType
 $Context.Response.ContentEncoding=[Text.Encoding]::UTF8
 $Context.Response.ContentLength64=$Bytes.Length
 try { $Context.Response.OutputStream.Write($Bytes,0,$Bytes.Length) } finally { $Context.Response.Close() }
}

function Write-BmWebJson([System.Net.HttpListenerContext]$Context,[int]$StatusCode,$Value) {
 $json=([string]($Value | ConvertTo-Json -Depth 6 -Compress))
 Write-BmWebResponse $Context $StatusCode 'application/json; charset=utf-8' ([Text.Encoding]::UTF8.GetBytes($json))
}

function Read-BmWebBody([System.Net.HttpListenerContext]$Context) {
 $max=16384
 $stream=$Context.Request.InputStream
 $buffer=[byte[]]::new(4096)
 $memory=New-Object IO.MemoryStream
 try {
  while (($read=$stream.Read($buffer,0,$buffer.Length)) -gt 0) {
   if ($memory.Length+$read -gt $max) { throw 'Request body too large.' }
   $memory.Write($buffer,0,$read)
  }
  return [Text.Encoding]::UTF8.GetString($memory.ToArray())
 } finally { $memory.Dispose() }
}

function Invoke-BmWebRequest([System.Net.HttpListenerContext]$Context) {
 $path=$Context.Request.Url.AbsolutePath
 try {
  if ($Context.Request.HttpMethod -eq 'GET' -and ($path -eq '/' -or $path -eq '/ble-test.html')) {
   $html=[IO.File]::ReadAllBytes((Join-Path $PSScriptRoot 'ble-test.html'))
   Write-BmWebResponse $Context 200 'text/html; charset=utf-8' $html
   return
  }
  if ($Context.Request.HttpMethod -eq 'GET' -and $path -eq '/api/state') {
   Write-BmWebJson $Context 200 ([pscustomobject]@{ok=$true;port=$script:Port;baud=$script:Baud;connected=[bool]$script:LinkConnected;logId=$script:WebLogId})
   return
  }
  if ($Context.Request.HttpMethod -eq 'GET' -and $path -eq '/api/events') {
   $after=0L
   if ($Context.Request.QueryString['after']) { $after=[long]$Context.Request.QueryString['after'] }
   $events=@($script:WebLog | Where-Object { $_.id -gt $after })
   Write-BmWebJson $Context 200 ([pscustomobject]@{ok=$true;events=$events;lastId=$script:WebLogId})
   return
  }
  if ($Context.Request.HttpMethod -eq 'POST' -and $path -eq '/api/action') {
   $payload=(Read-BmWebBody $Context | ConvertFrom-Json)
   $action=ConvertFrom-BmWebAction $payload
   Invoke-BmAction $action.Action $action.Argument $action.Format $action.Count $action.IntervalMs
   Write-BmWebJson $Context 200 ([pscustomobject]@{ok=$true;action=$action.Action})
   return
  }
  Write-BmWebJson $Context 404 ([pscustomobject]@{ok=$false;error='Not found.'})
 } catch {
  Write-BmWebJson $Context 400 ([pscustomobject]@{ok=$false;error=$_.Exception.Message})
 }
}

function Start-BmWebListener([int]$WebPort) {
$script:WebListener=[Net.HttpListener]::new()
$script:WebListener.Prefixes.Add("http://127.0.0.1:$WebPort/")
$script:WebListener.Start()
 $script:WebTask=$script:WebListener.GetContextAsync()
}

function Process-BmWebRequests {
 if ($null -eq $script:WebTask -or -not $script:WebTask.IsCompleted) { return }
 try { $context=$script:WebTask.GetAwaiter().GetResult() } catch { return }
 try {
  $script:WebTask=$script:WebListener.GetContextAsync()
  Invoke-BmWebRequest $context
 } catch {
  Write-BmLog ('WEB ERROR '+$_.Exception.Message)
}
}

function Stop-BmWebListener {
 if ($null -ne $script:WebListener) {
  try { $script:WebListener.Stop() } catch { }
  try { $script:WebListener.Close() } catch { }
  $script:WebListener=$null
 }
}

function Start-BmWeb {
 Start-BmWebListener $script:HttpPort
 Write-BmLog "WEB http://127.0.0.1:$($script:HttpPort)/"
 $clock=[Diagnostics.Stopwatch]::StartNew()
 try {
  while ($script:Seconds -eq 0 -or $clock.Elapsed.TotalSeconds -lt $script:Seconds) {
   $null=@(Read-BmFrames)
   Process-BmWebRequests
   Start-Sleep -Milliseconds 10
  }
 } finally { Stop-BmWebListener }
}
function Start-BmShell {
 Write-Host 'Commands: version | status | name TEXT | advertise on/off | power 0..15 | send HEX | text TEXT | counter | save | quit'
 Write-Host 'Incoming events remain visible while typing. Enter runs a command. Ctrl+C exits.'
 $line=''
 while ($true) {
  $null=@(Read-BmFrames)
  if ([Console]::KeyAvailable) {
   $key=[Console]::ReadKey($true)
   if ($key.Key -eq 'Enter') {
    Write-Host ''
    $parts=$line.Trim() -split '\s+',2
    $line=''
    if ($parts[0] -eq 'quit') { break }
    if ($parts[0]) {
     $arg=if ($parts.Count -gt 1) { $parts[1] } else { '' }
     if ($parts[0] -eq 'text') { Invoke-BmAction send $arg Text } else { Invoke-BmAction $parts[0] $arg }
    }
   } elseif ($key.Key -eq 'Backspace') {
    if ($line.Length) { $line=$line.Substring(0,$line.Length-1);Write-Host "`b `b" -NoNewline }
   } elseif (-not [char]::IsControl($key.KeyChar)) { $line+=$key.KeyChar;Write-Host $key.KeyChar -NoNewline }
  }
  Start-Sleep -Milliseconds 10
 }
}
if ($MyInvocation.InvocationName -eq '.') { return }
if ($Command -eq 'Ports') { [IO.Ports.SerialPort]::GetPortNames() | Sort-Object;return }
$script:Serial=$null
try {
 $script:Port=$Port;$script:Baud=$Baud;$script:HttpPort=$HttpPort;$script:Count=$Count;$script:IntervalMs=$IntervalMs;$script:Seconds=$Seconds
 $script:WebLog=[Collections.Generic.List[object]]::new();$script:WebLogId=0;$script:LinkConnected=$false
 if ($LogPath) { Add-Content -LiteralPath $LogPath -Value '# BM7701 session' -Encoding UTF8 }
 $script:Parser=New-BmParser
 $script:Clock=[Diagnostics.Stopwatch]::StartNew()
 $script:LastByteAt=0
 $script:Serial=[IO.Ports.SerialPort]::new($Port,$Baud,[IO.Ports.Parity]::None,8,[IO.Ports.StopBits]::One)
 $script:Serial.Handshake=[IO.Ports.Handshake]::None
 $script:Serial.DtrEnable=$false
 $script:Serial.RtsEnable=$false
 $script:Serial.ReadTimeout=$TimeoutMs
 $script:Serial.WriteTimeout=$TimeoutMs
 $script:Serial.Open()
 Write-BmLog "OPEN $Port $Baud 8N1; DTR/RTS disabled"
 switch ($Command) {
  'Shell' { Start-BmShell }
  'Receive' { if ($Seconds -eq 0) { while ($true) { Wait-BmData 100 } } else { Wait-BmData ($Seconds*1000) } }
  'Web' { Start-BmWeb }
  default { Invoke-BmAction $Command $Value $Encoding }
 }
} catch { Write-Error $_ -ErrorAction Continue;exit 1 }
finally { Stop-BmWebListener; if ($null -ne $script:Serial) { $script:Serial.Dispose() } }
