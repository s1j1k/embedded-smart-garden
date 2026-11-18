# Compile and flash the garden firmware to the ESP32 over USB serial.
#
#   .\scripts\flash.ps1                 # compile + upload, auto-detect COM port
#   .\scripts\flash.ps1 -Port COM3      # force a port
#   .\scripts\flash.ps1 -SkipCompile    # re-upload the last build in .\build
#   .\scripts\flash.ps1 -Monitor        # open the serial monitor after upload
#   .\scripts\flash.ps1 -NoPowerOff     # skip the MQTT power-off before upload
#   .\scripts\flash.ps1 -MqttHost 192.168.4.135   # broker other than secrets.h
#
# Before uploading, the garden is switched to standby over MQTT
# ({"cmd":"power","action":"off"}): uploads fail while it is ON. The board
# boots into standby after the flash, so press ON (or send power on) after.
#
# Uses the FQBN from sketch.yaml (PartitionScheme=no_ota; firmware needs the
# 2MB app partition). Build output goes to .\build so -SkipCompile can reuse it.
param(
    [string]$Port,
    [switch]$SkipCompile,
    [switch]$Monitor,
    [int]$Baud = 115200,
    [switch]$NoPowerOff,
    [string]$MqttHost
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $Root 'build'
$MosquittoDir = 'C:\Program Files\mosquitto'
$CmdTopic = 'smart-garden/garden-node-01/cmd'
$AckTopic = 'smart-garden/garden-node-01/ack'
$Fqbn = 'esp32:esp32:esp32:UploadSpeed=921600,CPUFreq=240,FlashFreq=80,FlashMode=qio,FlashSize=4M,PartitionScheme=no_ota,DebugLevel=none,PSRAM=disabled,LoopCore=1,EventsCore=1,EraseFlash=none,JTAGAdapter=default,ZigbeeMode=default'

if (-not (Get-Command arduino-cli -ErrorAction SilentlyContinue)) {
    throw 'arduino-cli not found on PATH'
}
if (-not (Test-Path (Join-Path $Root 'secrets.h'))) {
    throw 'secrets.h missing: copy secrets.h.example and fill in credentials'
}

# A string value from secrets.h (NAME = "value"), or '' if absent.
function Get-Secret([string]$Name) {
    $m = Select-String -Path (Join-Path $Root 'secrets.h') -Pattern ('{0}\s*=\s*"([^"]*)"' -f $Name) |
        Select-Object -First 1
    if ($m) { return $m.Matches[0].Groups[1].Value }
    return ''
}

# Sends {"cmd":"power","action":<Action>} and waits for the power ack.
# Returns the ack object, or $null if none came (board offline, broker down).
function Send-PowerCommand([string]$Action, [int]$TimeoutS = 15) {
    $pub = Join-Path $MosquittoDir 'mosquitto_pub.exe'
    $sub = Join-Path $MosquittoDir 'mosquitto_sub.exe'
    if (-not (Test-Path $pub) -or -not (Test-Path $sub)) {
        Write-Warning "Mosquitto clients not found in $MosquittoDir"
        return $null
    }
    $broker = if ($MqttHost) { $MqttHost } else { Get-Secret 'MQTT_BROKER_HOST' }
    if (-not $broker) { $broker = 'smar-gardon.local' }
    $auth = @()
    $user = Get-Secret 'MQTT_USERNAME'
    if ($user) { $auth = @('-u', $user, '-P', (Get-Secret 'MQTT_PASSWORD')) }

    # Payload from a file: Windows PowerShell strips the quotes from JSON
    # passed as a native argument.
    $payload = New-TemporaryFile
    $acks = New-TemporaryFile
    $errs = New-TemporaryFile
    $listener = $null
    try {
        Set-Content -Path $payload -Value ('{"cmd":"power","action":"' + $Action + '"}') -NoNewline -Encoding ascii
        $listener = Start-Process -FilePath $sub -NoNewWindow -PassThru `
            -ArgumentList (@('-h', $broker, '-p', '1883', '-t', $AckTopic, '-W', "$TimeoutS") + $auth) `
            -RedirectStandardOutput $acks -RedirectStandardError $errs
        Start-Sleep -Milliseconds 700   # let the subscription land first
        & $pub -h $broker -p 1883 -t $CmdTopic -f $payload @auth
        if ($LASTEXITCODE -ne 0) {
            Write-Warning "Could not publish to MQTT broker $broker"
            return $null
        }
        $deadline = (Get-Date).AddSeconds($TimeoutS)
        while ((Get-Date) -lt $deadline -and -not $listener.HasExited) {
            foreach ($line in @(Get-Content -Path $acks -ErrorAction SilentlyContinue)) {
                if ($line -match '"cmd"\s*:\s*"power"') { return ($line | ConvertFrom-Json) }
            }
            Start-Sleep -Milliseconds 250
        }
        return $null
    } finally {
        if ($listener -and -not $listener.HasExited) { $listener.Kill() }
        Remove-Item $payload, $acks, $errs -ErrorAction SilentlyContinue
    }
}

if (-not $Port) {
    $ports = (arduino-cli board list --format json | ConvertFrom-Json).detected_ports |
        Where-Object { $_.port.protocol -eq 'serial' } |
        ForEach-Object { $_.port.address }
    $ports = @($ports)
    if ($ports.Count -eq 0) { throw 'No serial port found. Is the ESP32 plugged in?' }
    if ($ports.Count -gt 1) { throw "Several serial ports found ($($ports -join ', ')); pass -Port COMx" }
    $Port = $ports[0]
}
Write-Host "Using port $Port"

if (-not $SkipCompile) {
    Write-Host 'Compiling...'
    arduino-cli compile --fqbn $Fqbn --warnings none --build-path $BuildDir $Root
    if ($LASTEXITCODE -ne 0) { throw "Compile failed (exit $LASTEXITCODE)" }
}

if (-not (Test-Path (Join-Path $BuildDir 'embedded-smart-garden.ino.bin'))) {
    throw "No build in $BuildDir. Run without -SkipCompile (or scripts/compile.sh)."
}
$bin = Get-Item (Join-Path $BuildDir 'embedded-smart-garden.ino.bin')
Write-Host "Image: $($bin.FullName) ($([math]::Round($bin.Length / 1KB)) KB, built $($bin.LastWriteTime))"

if (-not $NoPowerOff) {
    Write-Host 'Switching the garden to standby over MQTT...'
    $ack = Send-PowerCommand 'off'
    if ($ack -and $ack.ok) {
        Write-Host "Garden: $($ack.message)"
        Start-Sleep -Seconds 1   # let the pumps and grow light settle off
    } elseif ($ack) {
        Write-Warning "Power off refused: $($ack.message). If the upload fails, press OFF on the panel."
    } else {
        Write-Warning 'No power ack over MQTT. If the upload fails, press OFF on the panel.'
    }
}

Write-Host "Uploading to $Port..."
arduino-cli upload --fqbn $Fqbn -p $Port --input-dir $BuildDir $Root
if ($LASTEXITCODE -ne 0) {
    # The auto-reset into the bootloader sometimes misses: retry with BOOT held.
    Write-Host ''
    Write-Host "Upload failed (exit $LASTEXITCODE). Close any serial monitor on $Port." -ForegroundColor Yellow
    Write-Host 'Hold the BOOT button on the ESP32, then press Enter here (keep holding' -ForegroundColor Yellow
    Write-Host 'until "Connecting..." finishes / "Writing at" appears).' -ForegroundColor Yellow
    [void](Read-Host 'Press Enter to retry')
    arduino-cli upload --fqbn $Fqbn -p $Port --input-dir $BuildDir $Root
    if ($LASTEXITCODE -ne 0) {
        throw "Upload failed again (exit $LASTEXITCODE)."
    }
}
Write-Host 'Flash complete.'

if ($Monitor) {
    arduino-cli monitor -p $Port --config "baudrate=$Baud"
}
