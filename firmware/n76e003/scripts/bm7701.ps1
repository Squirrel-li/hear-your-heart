<#
.SYNOPSIS
 BM7701 BCI UART controller: one-shot commands, interactive shell and a localhost web console.
.DESCRIPTION
 Layers, top to bottom:
  Protocol  - pure frame encode/decode, no I/O (unit tested).
  Actions   - one validated job model shared by CLI, Shell and Web.
  Session   - serial I/O, logging and the blocking command runner (CLI/Shell).
  Web       - non-blocking job runner + HttpListener API serving ble-test.html.
  Shell     - interactive console.
 Dot-sourcing this file (as test-bm7701.ps1 does) loads the functions without opening a port.
#>
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

#region Protocol ---------------------------------------------------------------
# Host -> module: 77 LEN FLAG TYPE_LO TYPE_HI DATA...   (LEN = data + 3)
# Module -> host: 78 LEN STATUS TYPE_LO TYPE_HI DATA... (STATUS low nibble = error code)
$script:BmTypeStatus=0x0000
$script:BmTypeConfig=0x0010
$script:BmTypeVersion=0x0020
$script:BmTypeName=0x0005
$script:BmTypeAdvertise=0x0007
$script:BmTypePower=0x000B
$script:BmTypeNotify=0xFFF1   # host -> BLE client notification
$script:BmTypeWrite=0xFFF2    # BLE client write -> host (unsolicited)
$script:BmMaxPayload=127      # conservative library limit; no fragmentation until MTU is verified

function ConvertTo-BmBytes([string]$Text,[string]$Format='Hex') {
 if ($Format -eq 'Text') { return ,([Text.Encoding]::UTF8.GetBytes($Text)) }
 if ($Format -ne 'Hex') { throw "Encoding must be Hex or Text, not '$Format'." }
 $hex=$Text -replace '\s',''
 if ($hex.Length % 2 -ne 0 -or $hex -match '[^0-9a-fA-F]') { throw 'HEX needs complete byte pairs, e.g. 00 01 FF.' }
 $bytes=[byte[]]::new($hex.Length/2)
 for ($i=0;$i -lt $bytes.Length;$i++) { $bytes[$i]=[Convert]::ToByte($hex.Substring($i*2,2),16) }
 return ,$bytes
}

function Format-BmHex([byte[]]$Bytes) {
 if ($null -eq $Bytes -or $Bytes.Length -eq 0) { return '' }
 return [BitConverter]::ToString($Bytes).Replace('-',' ')
}

function Format-BmText([byte[]]$Bytes) { return ([Text.Encoding]::UTF8.GetString($Bytes) -replace '[\x00-\x1F\x7F]','.') }

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
  # Resynchronise on the reply header; LEN < 3 cannot hold STATUS+TYPE.
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
 return ($Frame.Type -ne $script:BmTypeWrite -and $Frame.Type -eq $Type -and ($Frame.Status -band 0xF0) -eq ($Flag -band 0xF0))
}

function Assert-BmSuccess($Frame) {
 if (($Frame.Status -band 15) -ne 0) { throw ('BM7701 error: type=0x{0:X4}, status=0x{1:X2}' -f $Frame.Type,$Frame.Status) }
}
#endregion

#region Actions ----------------------------------------------------------------
# A job is a validated command plus progress. Count > 1 only for 'counter'.
# Both the blocking runner (Invoke-BmJob) and the web runner (Step-BmWebJob) consume it.
$script:BmActionNames=@('version','status','name','advertise','power','send','counter','save')

function New-BmJob {
 param([string]$Action,[string]$Argument='',[string]$Format='Hex',[int]$Count=1,[int]$IntervalMs=100)
 $name=([string]$Action).ToLowerInvariant()
 $job=[pscustomobject]@{Name=$name;Type=0;Flag=0;Data=[byte[]]@();Count=1;IntervalMs=0;Index=0;Phase='ready';Deadline=0L;NextAt=0L}
 switch ($name) {
  'version' { $job.Type=$script:BmTypeVersion }
  'status' { $job.Type=$script:BmTypeStatus }
  'name' {
   $data=ConvertTo-BmBytes $Argument Text
   if ($data.Length -lt 1 -or $data.Length -gt 31 -or $Argument -match '[\x00-\x1F\x7F]') { throw 'Name must be 1..31 UTF-8 bytes without control characters.' }
   $job.Type=$script:BmTypeName;$job.Data=$data
  }
  'advertise' {
   if ($Argument -notin @('on','off')) { throw 'Advertise requires on or off.' }
   $job.Type=$script:BmTypeAdvertise;$job.Data=[byte[]]@([byte]($Argument -eq 'on'))
  }
  'power' {
   if ($Argument -notmatch '^(?:[0-9]|1[0-5])$') { throw 'Power must be 0..15.' }
   $job.Type=$script:BmTypePower;$job.Data=[byte[]]@([byte]$Argument)
  }
  'send' {
   $data=ConvertTo-BmBytes $Argument $Format
   if ($data.Length -lt 1 -or $data.Length -gt $script:BmMaxPayload) { throw "Send requires 1..$($script:BmMaxPayload) bytes; start with <=20 bytes." }
   $job.Type=$script:BmTypeNotify;$job.Flag=0x20;$job.Data=$data
  }
  'counter' {
   if ($Count -lt 1 -or $Count -gt 65536) { throw 'Counter count must be 1..65536.' }
   if ($IntervalMs -lt 10 -or $IntervalMs -gt 60000) { throw 'Counter interval must be 10..60000 ms.' }
   $job.Type=$script:BmTypeNotify;$job.Flag=0x20;$job.Count=$Count;$job.IntervalMs=$IntervalMs
  }
  'save' { $job.Type=$script:BmTypeConfig;$job.Flag=0x10;$job.Data=[byte[]]@(0x10,0,0,0) }
  default { throw "Unknown action: $Action" }
 }
 return $job
}

function Get-BmJobData($Job) {
 # Counter frame: 42 4D seq_lo seq_hi 00 01 02 03 04 05
 if ($Job.Name -eq 'counter') { return ,([byte[]]@(0x42,0x4D,($Job.Index -band 255),(($Job.Index -shr 8) -band 255),0,1,2,3,4,5)) }
 return ,([byte[]]$Job.Data)
}

function Get-BmJobSummary($Job) {
 switch ($Job.Name) {
  'counter' { return ('Counter complete: {0} UART ACKs; compare BLE client results.' -f $Job.Count) }
  'send' { return 'UART ACK only; verify delivery in the BLE client notification log.' }
  'save' { return 'Flash save requested. No automatic reset.' }
  default { return ('{0} complete.' -f $Job.Name) }
 }
}
#endregion

#region Session ----------------------------------------------------------------
function Initialize-BmSession {
 $script:WebLog=[Collections.Generic.List[object]]::new();$script:WebLogId=0
 $script:LinkConnected=$false;$script:WebAction=$null;$script:WebLastResult=$null
 $script:Parser=New-BmParser
 $script:Clock=[Diagnostics.Stopwatch]::StartNew();$script:LastByteAt=0
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
 if ($Frame.Type -eq $script:BmTypeWrite) {
  Write-BmLog ('BLE WRITE {0} | UTF8={1}' -f (Format-BmHex $Frame.Data),(Format-BmText $Frame.Data))
 } elseif ($Frame.Type -eq $script:BmTypeStatus -and $Frame.Data.Length -gt 0) {
  $script:LinkConnected=(($Frame.Data[0] -band 1) -ne 0)
  Write-BmLog ('LINK connected={0}; raw status={1}' -f $script:LinkConnected,(Format-BmHex $Frame.Data))
 }
}

function Read-BmFrames {
 # Drains the port, logs every frame and returns them so callers can look for ACKs.
 if (-not $script:Serial.IsOpen) { throw 'Serial port disconnected.' }
 $available=$script:Serial.BytesToRead
 if ($available -gt 0) {
  $bytes=[byte[]]::new($available)
  $n=$script:Serial.Read($bytes,0,$bytes.Length)
  if ($n -gt 0) {
   if ($n -lt $bytes.Length) { $bytes=[byte[]]$bytes[0..($n-1)] }
   Add-BmBytes $script:Parser $bytes
   $script:LastByteAt=$script:Clock.ElapsedMilliseconds
  }
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

function Find-BmReply($Frames,[int]$Type,[int]$Flag) {
 foreach ($frame in @($Frames)) { if (Test-BmReply $frame $Type $Flag) { return $frame } }
 return $null
}

function Send-BmFrame([int]$Type,[int]$Flag=0,[byte[]]$Data=@()) {
 $raw=New-BmFrame $Type $Flag $Data
 Write-BmLog ('TX '+(Format-BmHex $raw))
 $script:Serial.Write($raw,0,$raw.Length)
}

function Invoke-BmCommand([int]$Type,[int]$Flag=0,[byte[]]$Data=@()) {
 # One outstanding command; BCI has no transaction ID, so a timeout must end the session
 # rather than risk matching a late ACK to the next command.
 $null=@(Read-BmFrames)
 Send-BmFrame $Type $Flag $Data
 $deadline=$script:Clock.ElapsedMilliseconds+$script:TimeoutMs
 while ($script:Clock.ElapsedMilliseconds -lt $deadline) {
  $match=Find-BmReply @(Read-BmFrames) $Type $Flag
  if ($null -ne $match) { Assert-BmSuccess $match;return $match }
  Start-Sleep -Milliseconds 5
 }
 throw ('Timeout waiting for 0x{0:X4}; close session to avoid matching a late ACK to a new command.' -f $Type)
}

function Wait-BmData([int]$Milliseconds) {
 $deadline=$script:Clock.ElapsedMilliseconds+$Milliseconds
 do { $null=@(Read-BmFrames);Start-Sleep -Milliseconds 5 } while ($script:Clock.ElapsedMilliseconds -lt $deadline)
}

function Invoke-BmJob($Job) {
 for ($i=0;$i -lt $Job.Count;$i++) {
  if ($i -gt 0) { Wait-BmData $Job.IntervalMs }
  $Job.Index=$i
  $null=Invoke-BmCommand $Job.Type $Job.Flag (Get-BmJobData $Job)
 }
 $Job.Index=$Job.Count
 Write-BmLog (Get-BmJobSummary $Job)
}

function Invoke-BmAction([string]$Action,[string]$Argument='',[string]$Format='Hex',[int]$ActionCount=0,[int]$ActionIntervalMs=0) {
 if ($ActionCount -le 0) { $ActionCount=$script:Count }
 if ($ActionIntervalMs -le 0) { $ActionIntervalMs=$script:IntervalMs }
 # Validation happens in New-BmJob before anything is written to the port.
 Invoke-BmJob (New-BmJob $Action $Argument $Format $ActionCount $ActionIntervalMs)
}

function Open-BmSerial {
 $serial=[IO.Ports.SerialPort]::new($script:Port,$script:Baud,[IO.Ports.Parity]::None,8,[IO.Ports.StopBits]::One)
 $serial.Handshake=[IO.Ports.Handshake]::None
 $serial.DtrEnable=$false   # never toggle a possible Reset line
 $serial.RtsEnable=$false
 $serial.ReadTimeout=$script:TimeoutMs
 $serial.WriteTimeout=$script:TimeoutMs
 $serial.Open()
 return $serial
}
#endregion

#region Web --------------------------------------------------------------------
# The HTTP loop never blocks on the UART: POST /api/action queues a job and returns 202;
# the main loop advances it from received frames (Step-BmWebJob).

function ConvertFrom-BmWebAction($Payload) {
 if ($null -eq $Payload -or [string]::IsNullOrWhiteSpace([string]$Payload.action)) { throw 'API action is required.' }
 $action=([string]$Payload.action).ToLowerInvariant()
 if ($action -notin $script:BmActionNames) { throw "Unsupported API action: $action" }
 $format=if ([string]::IsNullOrWhiteSpace([string]$Payload.encoding)) { 'Hex' } else { [string]$Payload.encoding }
 if ($format -notin @('Hex','Text')) { throw 'API encoding must be Hex or Text.' }
 $argument=if ($null -eq $Payload.value) { '' } else { [string]$Payload.value }
 $count=[int]$script:Count;$interval=[int]$script:IntervalMs
 if (-not [string]::IsNullOrWhiteSpace([string]$Payload.count) -and -not [int]::TryParse([string]$Payload.count,[ref]$count)) { throw 'API count must be an integer.' }
 if (-not [string]::IsNullOrWhiteSpace([string]$Payload.intervalMs) -and -not [int]::TryParse([string]$Payload.intervalMs,[ref]$interval)) { throw 'API intervalMs must be an integer.' }
 if ($count -lt 1 -or $count -gt 65536) { throw 'API count must be 1..65536.' }
 if ($interval -lt 10 -or $interval -gt 60000) { throw 'API intervalMs must be 10..60000.' }
 [pscustomobject]@{Action=$action;Argument=$argument;Format=$format;Count=$count;IntervalMs=$interval}
}

function Start-BmWebAction($Action) {
 if ($null -ne $script:WebAction) { throw 'Another BM7701 web action is still running.' }
 $count=if ($Action.Action -eq 'counter') { $Action.Count } else { 1 }
 $script:WebAction=New-BmJob $Action.Action $Action.Argument $Action.Format $count $Action.IntervalMs
 Write-BmLog ('WEB ACTION queued: {0}' -f $Action.Action)
}

function Set-BmWebResult($Job,[bool]$Ok,[string]$Message) {
 $script:WebLastResult=[pscustomobject]@{action=$Job.Name;ok=$Ok;message=$Message;at=[DateTimeOffset]::Now.ToString('o')}
}

function Complete-BmWebAction($Job) {
 $message=Get-BmJobSummary $Job
 Write-BmLog $message
 Set-BmWebResult $Job $true $message
 $script:WebAction=$null
}

function Fail-BmWebAction($Job,[string]$Message) {
 Write-BmLog ('WEB ACTION ERROR {0}: {1}' -f $Job.Name,$Message)
 Set-BmWebResult $Job $false $Message
 # Short cooldown lets a late ACK arrive and be ignored, because BCI has no transaction ID.
 $script:WebAction=[pscustomobject]@{Name='cooldown';Phase='cooldown';Index=0;Count=1;NextAt=$script:Clock.ElapsedMilliseconds+$script:TimeoutMs}
}

function Send-BmWebCommand($Job) {
 Send-BmFrame $Job.Type $Job.Flag (Get-BmJobData $Job)
 $Job.Phase='wait';$Job.Deadline=$script:Clock.ElapsedMilliseconds+$script:TimeoutMs
}

function Process-BmWebAction($Frames) {
 # States: ready -> wait -> (delay -> wait)* -> done | cooldown -> idle
 $job=$script:WebAction
 if ($null -eq $job) { return }
 $now=$script:Clock.ElapsedMilliseconds
 switch ($job.Phase) {
  'cooldown' { if ($now -ge $job.NextAt) { $script:WebAction=$null };return }
  'ready' { Send-BmWebCommand $job;return }
  'delay' { if ($now -ge $job.NextAt) { Send-BmWebCommand $job };return }
  'wait' { }
  default { return }
 }
 $match=Find-BmReply $Frames $job.Type $job.Flag
 if ($null -ne $match) {
  try { Assert-BmSuccess $match } catch { Fail-BmWebAction $job $_.Exception.Message;return }
  $job.Index++
  if ($job.Index -ge $job.Count) { Complete-BmWebAction $job }
  else { $job.NextAt=$now+$job.IntervalMs;$job.Phase='delay' }
  return
 }
 if ($now -ge $job.Deadline) { Fail-BmWebAction $job ('Timeout waiting for 0x{0:X4}; action cancelled.' -f $job.Type) }
}

function Get-BmWebState {
 $job=$script:WebAction
 [pscustomobject]@{
  port=$script:Port;baud=$script:Baud;connected=[bool]$script:LinkConnected
  busy=($null -ne $job)
  action=if ($null -eq $job) { $null } else { $job.Name }
  done=if ($null -eq $job) { 0 } else { [int]$job.Index }
  total=if ($null -eq $job) { 0 } else { [int]$job.Count }
  last=$script:WebLastResult
  logId=$script:WebLogId
 }
}

function Get-BmWebEvents([long]$After,[int]$Max=500) {
 # Ids are contiguous, so slice instead of scanning the whole log every poll.
 $log=$script:WebLog
 if ($null -eq $log -or $log.Count -eq 0) { return ,@() }
 $start=[long]$After-[long]$log[0].id+1
 if ($start -lt 0) { $start=0 }
 if ($start -ge $log.Count) { return ,@() }
 return ,($log.GetRange([int]$start,[Math]::Min($Max,$log.Count-[int]$start)).ToArray())
}

function Test-BmWebOrigin([string]$Origin,[string]$FetchSite) {
 # Blocks other websites from driving the UART (CSRF) through this localhost API.
 if ($FetchSite -eq 'cross-site') { return $false }
 if ([string]::IsNullOrEmpty($Origin)) { return $true }
 return ($Origin -in @("http://127.0.0.1:$($script:HttpPort)","http://localhost:$($script:HttpPort)"))
}

function Write-BmWebResponse([System.Net.HttpListenerContext]$Context,[int]$StatusCode,[string]$ContentType,[byte[]]$Bytes) {
 $response=$Context.Response
 $response.StatusCode=$StatusCode
 $response.ContentType=$ContentType
 $response.ContentEncoding=[Text.Encoding]::UTF8
 $response.Headers['Cache-Control']='no-store'
 $response.Headers['X-Content-Type-Options']='nosniff'
 $response.ContentLength64=$Bytes.Length
 try { $response.OutputStream.Write($Bytes,0,$Bytes.Length) } finally { $response.Close() }
}

function Write-BmWebJson([System.Net.HttpListenerContext]$Context,[int]$StatusCode,$Value) {
 $json=[string]($Value | ConvertTo-Json -Depth 6 -Compress)
 Write-BmWebResponse $Context $StatusCode 'application/json; charset=utf-8' ([Text.Encoding]::UTF8.GetBytes($json))
}

function Read-BmWebBody([System.Net.HttpListenerContext]$Context) {
 $max=16384
 if ($Context.Request.ContentLength64 -gt $max) { throw 'Request body too large.' }
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
 $request=$Context.Request
 $route='{0} {1}' -f $request.HttpMethod,$request.Url.AbsolutePath
 try {
  if (-not (Test-BmWebOrigin $request.Headers['Origin'] $request.Headers['Sec-Fetch-Site'])) {
   Write-BmWebJson $Context 403 ([pscustomobject]@{ok=$false;error='Cross-origin request rejected.'});return
  }
  switch -Exact ($route) {
   { $_ -in @('GET /','GET /ble-test.html') } {
    Write-BmWebResponse $Context 200 'text/html; charset=utf-8' ([IO.File]::ReadAllBytes($script:WebPagePath));return
   }
   'GET /api/state' { Write-BmWebJson $Context 200 ([pscustomobject]@{ok=$true;session=$script:WebSession;state=(Get-BmWebState)});return }
   'GET /api/events' {
    $after=0L;$null=[long]::TryParse([string]$request.QueryString['after'],[ref]$after)
    $events=Get-BmWebEvents $after
    Write-BmWebJson $Context 200 ([pscustomobject]@{ok=$true;session=$script:WebSession;events=$events;lastId=$script:WebLogId;state=(Get-BmWebState)});return
   }
   'POST /api/action' {
    # Requiring JSON forces a CORS preflight for cross-origin callers, which this server never approves.
    if ([string]$request.ContentType -notmatch '^application/json\b') {
     Write-BmWebJson $Context 415 ([pscustomobject]@{ok=$false;error='Content-Type must be application/json.'});return
    }
    $action=ConvertFrom-BmWebAction (Read-BmWebBody $Context | ConvertFrom-Json)
    Start-BmWebAction $action
    Write-BmWebJson $Context 202 ([pscustomobject]@{ok=$true;action=$action.Action;queued=$true});return
   }
  }
  Write-BmWebJson $Context 404 ([pscustomobject]@{ok=$false;error='Not found.'})
 } catch {
  Write-BmWebJson $Context 400 ([pscustomobject]@{ok=$false;error=$_.Exception.Message})
 }
}

function Start-BmWebListener([int]$WebPort) {
 $script:WebListener=[Net.HttpListener]::new()
 $script:WebListener.Prefixes.Add("http://127.0.0.1:$WebPort/")
 $script:WebListener.Prefixes.Add("http://localhost:$WebPort/")
 $script:WebListener.Start()
 $script:WebTask=$script:WebListener.GetContextAsync()
}

function Process-BmWebRequests([int]$Max=8) {
 for ($i=0;$i -lt $Max;$i++) {
  if ($null -eq $script:WebTask -or -not $script:WebTask.IsCompleted) { return }
  try { $context=$script:WebTask.GetAwaiter().GetResult() } catch { $context=$null }
  $script:WebTask=$script:WebListener.GetContextAsync()
  if ($null -eq $context) { continue }
  try { Invoke-BmWebRequest $context } catch { Write-BmLog ('WEB ERROR '+$_.Exception.Message) }
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
 $script:WebPagePath=Join-Path $PSScriptRoot 'ble-test.html'
 if (-not (Test-Path -LiteralPath $script:WebPagePath)) { throw "Web page not found: $($script:WebPagePath)" }
 $script:WebSession=[guid]::NewGuid().ToString('N')
 Start-BmWebListener $script:HttpPort
 Write-BmLog "WEB http://127.0.0.1:$($script:HttpPort)/"
 $clock=[Diagnostics.Stopwatch]::StartNew()
 try {
  while ($script:Seconds -eq 0 -or $clock.Elapsed.TotalSeconds -lt $script:Seconds) {
   $frames=@(Read-BmFrames)
   Process-BmWebRequests
   Process-BmWebAction $frames
   Start-Sleep -Milliseconds 10
  }
 } finally { Stop-BmWebListener }
}
#endregion

#region Shell ------------------------------------------------------------------
$script:BmShellHelp='Commands: version | status | name TEXT | advertise on/off | power 0..15 | send HEX | text TEXT | counter [N] | save | help | quit'

function ConvertFrom-BmShellLine([string]$Line) {
 # Returns $null for an empty line, 'quit'/'help' strings, or a validated job.
 $parts=@($Line.Trim() -split '\s+',2)
 $verb=$parts[0].ToLowerInvariant()
 $arg=if ($parts.Count -gt 1) { $parts[1] } else { '' }
 switch ($verb) {
  '' { return $null }
  { $_ -in @('quit','exit') } { return 'quit' }
  { $_ -in @('help','?') } { return 'help' }
  'text' { return New-BmJob send $arg Text }
  'counter' {
   $n=$script:Count
   if ($arg -and -not [int]::TryParse($arg,[ref]$n)) { throw 'counter takes an optional count, e.g. counter 100.' }
   return New-BmJob counter '' Hex $n $script:IntervalMs
  }
  default { return New-BmJob $verb $arg Hex $script:Count $script:IntervalMs }
 }
}

function Start-BmShell {
 Write-Host $script:BmShellHelp
 Write-Host 'Incoming events remain visible while typing. Input errors are reported; device errors or timeouts close the session.'
 $line=''
 while ($true) {
  $null=@(Read-BmFrames)
  if ([Console]::KeyAvailable) {
   $key=[Console]::ReadKey($true)
   if ($key.Key -eq 'Enter') {
    Write-Host ''
    $text=$line;$line=''
    try { $job=ConvertFrom-BmShellLine $text } catch { Write-Host ('INPUT ERROR: '+$_.Exception.Message) -ForegroundColor Yellow;continue }
    if ($job -is [string]) {
     if ($job -eq 'quit') { break }
     Write-Host $script:BmShellHelp;continue
    }
    if ($null -ne $job) { Invoke-BmJob $job }
   } elseif ($key.Key -eq 'Backspace') {
    if ($line.Length) { $line=$line.Substring(0,$line.Length-1);Write-Host "`b `b" -NoNewline }
   } elseif (-not [char]::IsControl($key.KeyChar)) { $line+=$key.KeyChar;Write-Host $key.KeyChar -NoNewline }
  }
  Start-Sleep -Milliseconds 10
 }
}
#endregion

#region Entry ------------------------------------------------------------------
if ($MyInvocation.InvocationName -eq '.') { return }
if ($Command -eq 'Ports') { [IO.Ports.SerialPort]::GetPortNames() | Sort-Object;return }
$script:Serial=$null
try {
 $script:Port=$Port;$script:Baud=$Baud;$script:HttpPort=$HttpPort;$script:TimeoutMs=$TimeoutMs
 $script:Count=$Count;$script:IntervalMs=$IntervalMs;$script:Seconds=$Seconds;$script:LogPath=$LogPath
 # Validate one-shot commands before touching the port.
 $job=if ($Command -in @('Shell','Receive','Web')) { $null } else { New-BmJob $Command $Value $Encoding $Count $IntervalMs }
 Initialize-BmSession
 if ($LogPath) { Add-Content -LiteralPath $LogPath -Value '# BM7701 session' -Encoding UTF8 }
 $script:Serial=Open-BmSerial
 Write-BmLog "OPEN $Port $Baud 8N1; DTR/RTS disabled"
 switch ($Command) {
  'Shell' { Start-BmShell }
  'Receive' { if ($Seconds -eq 0) { while ($true) { Wait-BmData 100 } } else { Wait-BmData ($Seconds*1000) } }
  'Web' { Start-BmWeb }
  default { Invoke-BmJob $job }
 }
} catch { Write-Error $_ -ErrorAction Continue;exit 1 }
finally { Stop-BmWebListener; if ($null -ne $script:Serial) { $script:Serial.Dispose() } }
#endregion
