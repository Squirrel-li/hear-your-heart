param([string]$SdccHome = 'C:/Program Files (x86)/Nuvoton Tools/Toolchain/SDCC')
$ErrorActionPreference = 'Stop'
$env:SDCC_HOME = $SdccHome
$env:PATH = "$SdccHome/bin;" + $env:PATH
$cc = Join-Path $SdccHome 'bin/sdcc.exe'
$bsp = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../N76E003_BSP/Library'))
$out = Join-Path $PSScriptRoot 'Release'
New-Item -ItemType Directory -Force $out | Out-Null
$options = @('-D__SDCC__', '--model-large', "-I$SdccHome/include", "-I$bsp/Device/Include", "-I$bsp/StdDriver/inc")
& $cc -c @options -o "$out/main.rel" "$PSScriptRoot/source/main.c"
if ($LASTEXITCODE) { throw 'Motion HR compilation failed' }
& $cc -c @options -o "$out/common.rel" "$bsp/StdDriver/src/common.c"
if ($LASTEXITCODE) { throw 'BSP compilation failed' }
& $cc --model-large "-L$SdccHome/lib/large" --xram-size 768 --code-size 18432 -o "$out/ble_sensor_motion.hex" "$out/main.rel" "$out/common.rel"
if ($LASTEXITCODE) { throw 'Link failed / device memory exceeded' }
Get-Content "$out/ble_sensor_motion.mem" | Select-Object -Last 6
Write-Output "Firmware: $out/ble_sensor_motion.hex (not flashed)"
