#Requires -Version 5.1
<#
  build.ps1 — Compila e grava o firmware ESP32-DIV (V2 / ESP32-S3).

  Uso:
    powershell -ExecutionPolicy Bypass -File .\build.ps1               # compila E grava na COM5
    powershell -ExecutionPolicy Bypass -File .\build.ps1 -CompileOnly  # so compila (nao grava)
    powershell -ExecutionPolicy Bypass -File .\build.ps1 -Port COM3    # grava em outra porta
    powershell -ExecutionPolicy Bypass -File .\build.ps1 -Clean        # build limpo (recompila tudo)

  (ou use o atalho build.cmd, que chama este script)
#>

param(
    [string]$Port = "COM5",   # porta serial da placa (CP210x da ESP32-DIV V2)
    [switch]$CompileOnly,     # nao faz o upload
    [switch]$Clean            # apaga o build anterior e recompila tudo
)

$ErrorActionPreference = "Stop"

# ---------------------------------------------------------------------------
# Caminhos (relativos a este script, que fica na raiz do repositorio)
# ---------------------------------------------------------------------------
$Workspace    = Split-Path -Parent $MyInvocation.MyCommand.Path
$Sketch       = Join-Path $Workspace "ESP32-DIV\ESP32-DIV.ino"
$BuildDir     = Join-Path $Workspace "build"
$ArtifactsDir = Join-Path $Workspace "build\artifacts"

# ---------------------------------------------------------------------------
# FQBN exato usado pelo build oficial (flash 16M, PSRAM octal, particao fat).
# ---------------------------------------------------------------------------
$Fqbn = 'esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,DebugLevel=none,PSRAM=opi,LoopCore=1,EventsCore=1,EraseFlash=none,JTAGAdapter=default'

# ---------------------------------------------------------------------------
# Flag de linker OBRIGATORIO: o deauther (wifi.cpp) redefine
# ieee80211_raw_frame_sanity_check, que colide com a libnet80211.a do core.
# Sem esse flag o link falha com "multiple definition".
# ---------------------------------------------------------------------------
$LinkerFlag = 'compiler.c.elf.extra_flags=-Wl,--allow-multiple-definition'

# ---------------------------------------------------------------------------
# Localiza o arduino-cli (embutido no Arduino IDE 2.x, ou no PATH).
# ---------------------------------------------------------------------------
function Find-ArduinoCli {
    $candidates = @(
        "C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe",
        "${env:ProgramFiles(x86)}\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe",
        "$env:LOCALAPPDATA\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return $c }
    }
    $fromPath = Get-Command arduino-cli -ErrorAction SilentlyContinue
    if ($fromPath) { return $fromPath.Source }
    return $null
}

$Cli = Find-ArduinoCli
if (-not $Cli) {
    Write-Host "[ERRO] arduino-cli nao encontrado. Instale o Arduino IDE 2.x." -ForegroundColor Red
    exit 1
}
if (-not (Test-Path $Sketch)) {
    Write-Host "[ERRO] Sketch nao encontrado: $Sketch" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "=== ESP32-DIV build ===" -ForegroundColor Cyan
Write-Host "arduino-cli : $Cli"
Write-Host "sketch      : $Sketch"
Write-Host "porta       : $Port"
Write-Host "FQBN        : $Fqbn"
Write-Host ""

# ---------------------------------------------------------------------------
# Limpeza opcional
# ---------------------------------------------------------------------------
if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Limpando build anterior..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $BuildDir
}
New-Item -ItemType Directory -Force -Path $BuildDir     | Out-Null
New-Item -ItemType Directory -Force -Path $ArtifactsDir | Out-Null

# ---------------------------------------------------------------------------
# Compilar
# ---------------------------------------------------------------------------
Write-Host "=== Compilando... ===" -ForegroundColor Cyan
& $Cli compile `
    --fqbn $Fqbn `
    --build-property $LinkerFlag `
    --build-path $BuildDir `
    --output-dir $ArtifactsDir `
    $Sketch

if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "[FALHA] Compilacao terminou com erro (exit $LASTEXITCODE)." -ForegroundColor Red
    exit $LASTEXITCODE
}
Write-Host "[OK] Compilacao concluida." -ForegroundColor Green

$Bin = Join-Path $ArtifactsDir "ESP32-DIV.ino.bin"
if (Test-Path $Bin) {
    $sz = [math]::Round((Get-Item $Bin).Length / 1KB, 1)
    Write-Host ("Firmware: {0} ({1} KB)" -f $Bin, $sz) -ForegroundColor Green
}

if ($CompileOnly) {
    Write-Host "CompileOnly: upload ignorado." -ForegroundColor Yellow
    exit 0
}

# ---------------------------------------------------------------------------
# Gravar
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "=== Gravando em ${Port}... ===" -ForegroundColor Cyan
& $Cli upload `
    -p $Port `
    --fqbn $Fqbn `
    --input-dir $ArtifactsDir `
    $Sketch

if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "[FALHA] Upload terminou com erro (exit $LASTEXITCODE)." -ForegroundColor Red
    Write-Host "Verifique se a placa esta conectada em $Port e tente novamente." -ForegroundColor Yellow
    exit $LASTEXITCODE
}

Write-Host ""
Write-Host "[OK] Firmware gravado em $Port. A placa foi reiniciada." -ForegroundColor Green
exit 0
