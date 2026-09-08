$ErrorActionPreference='Stop'
. "$PSScriptRoot\bm7701.ps1"
function Assert($Condition,[string]$Message) { if (-not $Condition) { throw "FAIL: $Message" } }
function Reject([scriptblock]$Code) { $failed=$false;try { & $Code } catch { $failed=$true };Assert $failed 'invalid input was accepted' }
Assert ((Format-BmHex (New-BmFrame 0x20)) -eq '77 03 00 20 00') 'version frame'
Assert ((Format-BmHex (New-BmFrame 0xFFF1 0x20 ([byte[]]@(0,1,2)))) -eq '77 06 20 F1 FF 00 01 02') 'notify frame, no legacy B2 prefix'
Assert ((Format-BmHex (New-BmFrame 0x10 0x10 ([byte[]]@(16,0,0,0)))) -eq '77 07 10 10 00 10 00 00 00') 'explicit flash save'
Assert ((Format-BmHex (ConvertTo-BmBytes '00 FF 78')) -eq '00 FF 78') 'hex'
Assert ((ConvertTo-BmBytes ([string][char]0x5FC3) Text).Length -eq 3) 'UTF8 byte length'
Reject { ConvertTo-BmBytes '0' };Reject { ConvertTo-BmBytes 'GG' };Reject { ConvertTo-BmBytes '0xFF' }
Reject { New-BmFrame 65536 };Reject { New-BmFrame 1 0 ([byte[]]::new(253)) }
Reject { Invoke-BmAction name ('x'*32) };Reject { Invoke-BmAction power '16' };Reject { Invoke-BmAction advertise 'yes' }
Reject { Invoke-BmAction send '' };Reject { Invoke-BmAction send ('AB'*128) }
$script:Count=100;$script:IntervalMs=100
$webAction=ConvertFrom-BmWebAction ([pscustomobject]@{action='send';value='00 FF';encoding='Hex'})
Assert ($webAction.Action -eq 'send' -and $webAction.Argument -eq '00 FF' -and $webAction.Format -eq 'Hex') 'web action parsing'
Reject { ConvertFrom-BmWebAction ([pscustomobject]@{action='shell'}) }
Reject { ConvertFrom-BmWebAction ([pscustomobject]@{action='send';encoding='Base64'}) }
Reject { ConvertFrom-BmWebAction ([pscustomobject]@{action='counter';count=0}) }
Reject { ConvertFrom-BmWebAction ([pscustomobject]@{action='counter';intervalMs=9}) }
# All possible two-chunk boundaries, payload containing header byte, and interleaved asynchronous data.
$stream=[byte[]]@(0,255,0x78,2,0,0x78,4,0,0xF2,0xFF,0x78,0x78,3,0x20,0xF1,0xFF,0x78,4,0,0,0,1)
for ($split=1;$split -lt $stream.Length;$split++) {
 $p=New-BmParser
 Add-BmBytes $p ([byte[]]$stream[0..($split-1)])
 Add-BmBytes $p ([byte[]]$stream[$split..($stream.Length-1)])
 Assert ($p.Frames.Count -eq 3) "split/concatenated boundary $split"
 $data=$p.Frames.Dequeue();$ack=$p.Frames.Dequeue();$status=$p.Frames.Dequeue()
 Assert ($data.Type -eq 0xFFF2 -and $data.Data[0] -eq 0x78) 'payload header preserved'
 Assert (-not (Test-BmReply $data 0xFFF1 0x20)) 'async data not consumed as ACK'
 Assert (Test-BmReply $ack 0xFFF1 0x20) 'matching ACK'
 Assert (-not (Test-BmReply $ack 0xFFF1 0)) 'flag mismatch'
 Assert ($status.Type -eq 0 -and $status.Data[0] -eq 1) 'following status retained'
 Assert ($p.Buffer.Count -eq 0) 'buffer drained'
}
$p=New-BmParser;Add-BmBytes $p ([byte[]]@(0x78,5,0))
Assert ($p.Frames.Count -eq 0 -and $p.Buffer.Count -eq 3) 'incomplete frame retained'
Reject { Assert-BmSuccess ([pscustomobject]@{Type=0xFFF1;Status=0x21}) }
Assert-BmSuccess ([pscustomobject]@{Type=0xFFF1;Status=0x20})
Write-Host 'PASS: frame encoding, validation, every split boundary, concatenated/interleaved events, incomplete frames and error replies. No hardware used.'
# Exercise the actual transaction loop with a fake SerialPort, not just its parser.
$script:Clock=[Diagnostics.Stopwatch]::StartNew();$script:LastByteAt=0;$script:TimeoutMs=100
$script:Parser=New-BmParser;$script:Observed=[Collections.Generic.List[object]]::new()
function Show-BmFrame($Frame) { $script:Observed.Add($Frame) }
$script:Serial=[pscustomobject]@{IsOpen=$true;Pending=[byte[]]@();Reply=[byte[]]@(0x78,4,0,0xF2,0xFF,0xAB,0x78,3,0x20,0xF1,0xFF,0x78,4,0,0xF2,0xFF,0xCD)}
$script:Serial | Add-Member ScriptProperty BytesToRead { $this.Pending.Length }
$script:Serial | Add-Member ScriptMethod Read { param($buffer,$offset,$size);$n=[Math]::Min($size,$this.Pending.Length);[Array]::Copy($this.Pending,0,$buffer,$offset,$n);$this.Pending=[byte[]]@();return $n }
$script:Serial | Add-Member ScriptMethod Write { param($buffer,$offset,$size);$this.Pending=$this.Reply }
$ack=Invoke-BmCommand 0xFFF1 0x20 ([byte[]]@(0,1))
Assert ($ack.Type -eq 0xFFF1 -and $script:Observed.Count -eq 3) 'transaction retains events before and after ACK'
Assert ($script:Observed[0].Data[0] -eq 0xAB -and $script:Observed[2].Data[0] -eq 0xCD) 'transaction event bytes'
$script:Serial.Reply=[byte[]]@(0x78,3,0x21,0xF1,0xFF)
Reject { Invoke-BmCommand 0xFFF1 0x20 ([byte[]]@(0)) }
$script:Serial.Reply=[byte[]]@()
Reject { Invoke-BmCommand 0x20 }
Add-BmBytes $script:Parser ([byte[]]@(0x78,20,0))
$script:LastByteAt=$script:Clock.ElapsedMilliseconds-200
$null=@(Read-BmFrames)
Assert ($script:Parser.Buffer.Count -eq 0) 'expired partial packet cleared'
Write-Host 'PASS: actual transaction loop preserves asynchronous data, rejects device errors, times out and expires partial frames.'

